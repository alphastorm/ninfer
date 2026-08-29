[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$StateProtectionPath,

    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$GpuOwnerControllerPath,

    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$InstallerPath,

    [string]$ReceiptPath,

    [string]$ManagedStateRoot,

    [switch]$ExerciseManagedRollback,

    [switch]$InstrumentGpuPowerFixture
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$principal = [Security.Principal.WindowsPrincipal]::new(
    [Security.Principal.WindowsIdentity]::GetCurrent()
)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    [ordered]@{
        artifact_type = 'ninfer_windows_state_security_regression'
        schema_version = 1
        status = 'skipped_requires_elevation'
    } | ConvertTo-Json -Compress
    exit 77
}
if ($ExerciseManagedRollback -and [string]::IsNullOrWhiteSpace($ManagedStateRoot)) {
    throw '-ExerciseManagedRollback requires -ManagedStateRoot'
}

$StateProtectionPath = (Resolve-Path -LiteralPath $StateProtectionPath).Path
$GpuOwnerControllerPath = (Resolve-Path -LiteralPath $GpuOwnerControllerPath).Path
$InstallerPath = (Resolve-Path -LiteralPath $InstallerPath).Path
. $StateProtectionPath

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}
function Assert-Rejected([scriptblock]$Action, [string]$Pattern, [string]$Message) {
    $rejected = $false
    try { & $Action }
    catch { $rejected = $_.Exception.Message -like $Pattern }
    Assert-True $rejected $Message
}
function Write-JsonAtomic([string]$Path, [object]$Value) {
    $temporary = "$Path.$([Guid]::NewGuid().ToString('N')).tmp"
    try {
        [IO.File]::WriteAllText($temporary, ($Value | ConvertTo-Json -Depth 12), [Text.UTF8Encoding]::new($false))
        Move-Item -LiteralPath $temporary -Destination $Path -Force
    }
    finally { Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue }
}

if (-not ('NInferSecurityNative' -as [type])) {
    Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class NInferSecurityNative {
    [DllImport("advapi32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    public static extern bool LogonUser(
        string username, string domain, string password,
        int logonType, int logonProvider, out IntPtr token);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool CloseHandle(IntPtr handle);
}
'@
}

function Get-LowPrivilegeFileDenials(
    [string]$Path,
    [string]$User,
    [string]$Password
) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "effective-access target is missing: $Path"
    }
    $token = [IntPtr]::Zero
    if (-not [NInferSecurityNative]::LogonUser(
            $User, $env:COMPUTERNAME, $Password, 2, 0, [ref]$token
        )) {
        throw "failed to log on the low-privilege effective-access principal: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
    }
    $readDenied = $false
    $writeDenied = $false
    $impersonation = $null
    try {
        $impersonation = [Security.Principal.WindowsIdentity]::Impersonate($token)
        $stream = $null
        try {
            $stream = [IO.File]::Open(
                $Path, [IO.FileMode]::Open, [IO.FileAccess]::Read,
                [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete
            )
        }
        catch [UnauthorizedAccessException] { $readDenied = $true }
        finally { if ($null -ne $stream) { $stream.Dispose() } }

        $stream = $null
        try {
            $stream = [IO.File]::Open(
                $Path, [IO.FileMode]::Open, [IO.FileAccess]::Write,
                [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete
            )
        }
        catch [UnauthorizedAccessException] { $writeDenied = $true }
        finally { if ($null -ne $stream) { $stream.Dispose() } }
    }
    finally {
        if ($null -ne $impersonation) { $impersonation.Undo(); $impersonation.Dispose() }
        [NInferSecurityNative]::CloseHandle($token) | Out-Null
    }
    return [pscustomobject]@{ read_denied = $readDenied; write_denied = $writeDenied }
}

$originalProgramData = $env:ProgramData
$testRoot = Join-Path $originalProgramData ('NInferSecurityRegression-' + [Guid]::NewGuid().ToString('N'))
$user = 'NfAcl' + [Guid]::NewGuid().ToString('N').Substring(0, 8)
$password = 'Nf!9aA' + [Guid]::NewGuid().ToString('N').Substring(0, 14)
$userCreated = $false
$gpuRoot = $null
try {
    $testRoot = Initialize-NInferProtectedStateRoot $testRoot
    $env:ProgramData = $testRoot
    $cleanDefaultParent = Join-Path $env:ProgramData 'NInfer'
    $cleanDefaultRoot = Join-Path $cleanDefaultParent 'qwen38-3090'
    Assert-True (-not (Test-Path -LiteralPath $cleanDefaultParent)) 'clean default managed parent fixture already exists'
    $cleanDefaultRoot = Initialize-NInferProtectedStateRoot $cleanDefaultRoot
    Assert-NInferProtectedAcl $cleanDefaultParent $true
    Assert-NInferProtectedStateTree $cleanDefaultRoot

    $racedRoot = Join-Path $env:ProgramData 'atomic-race-winner'
    New-Item -ItemType Directory -Path $racedRoot | Out-Null
    $racedMarker = Join-Path $racedRoot 'attacker-marker.txt'
    [IO.File]::WriteAllText($racedMarker, 'preserve', [Text.UTF8Encoding]::new($false))
    Assert-Rejected { New-NInferProtectedDirectoryAtomic $racedRoot } `
        '*already exists*' 'atomic creation accepted a raced precreated directory'
    Assert-True (Test-Path -LiteralPath $racedMarker -PathType Leaf) `
        'atomic creation failure deleted raced precreated state'
    $env:ProgramData = $originalProgramData

    & net.exe user $user $password '/add' '/y' | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'failed to create the low-privilege effective-access principal' }
    $userCreated = $true

    $protected = Initialize-NInferProtectedStateRoot (Join-Path $testRoot 'protected')
    Assert-NInferProtectedStateTree $protected
    $rootAcl = Get-Acl -LiteralPath $protected
    $rootOwner = ConvertFrom-NInferOwnerString ([string]$rootAcl.Owner)
    Assert-True ($rootOwner -cin @('S-1-5-18', 'S-1-5-32-544')) 'protected root owner is not SYSTEM or Administrators'
    Assert-True $rootAcl.AreAccessRulesProtected 'protected root DACL still inherits'

    $adminProbe = Join-Path $protected 'admin-write.txt'
    [IO.File]::WriteAllText($adminProbe, 'admin', [Text.UTF8Encoding]::new($false))
    $protectedDenials = Get-LowPrivilegeFileDenials $adminProbe $user $password
    Assert-True ([bool]$protectedDenials.read_denied -and [bool]$protectedDenials.write_denied) `
        'protected root allowed real low-privilege read or write access'

    $nullDaclPath = Join-Path $protected 'null-dacl.txt'
    [IO.File]::WriteAllText($nullDaclPath, 'must remain protected', [Text.UTF8Encoding]::new($false))
    $nullDacl = [Security.AccessControl.FileSecurity]::new()
    $nullDacl.SetSecurityDescriptorSddlForm('D:NO_ACCESS_CONTROL')
    Set-Acl -LiteralPath $nullDaclPath -AclObject $nullDacl
    Assert-Rejected { Assert-NInferProtectedStateTree $protected } `
        '*NULL DACL*' 'protected state accepted a NULL DACL file'
    Set-NInferProtectedFileAcl $nullDaclPath

    $precreated = Join-Path $testRoot 'precreated'
    New-Item -ItemType Directory -Path $precreated | Out-Null
    Assert-Rejected { Initialize-NInferProtectedStateRoot $precreated | Out-Null } `
        '*protected state inherits an external DACL*' 'attacker-precreated root was accepted'

    $unowned = Join-Path $testRoot 'unowned'
    New-Item -ItemType Directory -Path $unowned | Out-Null
    $unownedAcl = Get-Acl -LiteralPath $unowned
    $unownedAcl.SetOwner([Security.Principal.NTAccount]::new("$env:COMPUTERNAME\$user"))
    Set-Acl -LiteralPath $unowned -AclObject $unownedAcl
    Assert-Rejected { Initialize-NInferProtectedStateRoot $unowned | Out-Null } `
        '*owner is not SYSTEM or Administrators*' 'unowned state root was accepted'

    $junctionTarget = Join-Path $testRoot 'junction-target'
    $junctionRoot = Join-Path $testRoot 'junction-root'
    New-Item -ItemType Directory -Path $junctionTarget | Out-Null
    & cmd.exe /d /c "mklink /J `"$junctionRoot`" `"$junctionTarget`"" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'failed to create the root-junction fixture' }
    Assert-Rejected { Initialize-NInferProtectedStateRoot $junctionRoot | Out-Null } `
        '*traverses a reparse point*' 'junction state root was accepted'

    $dummyPackage = Join-Path $testRoot 'dummy-package.zip'
    $dummyModel = Join-Path $testRoot 'dummy-model.ninfer'
    $dummyKey = Join-Path $testRoot 'dummy-key.txt'
    [IO.File]::WriteAllText($dummyPackage, 'not-a-package', [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText($dummyModel, 'not-a-model', [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText($dummyKey, "fixture-key`n", [Text.UTF8Encoding]::new($false))
    $dummyPackageSha = (Get-FileHash -Algorithm SHA256 -LiteralPath $dummyPackage).Hash.ToLowerInvariant()
    foreach ($fixture in @(
            [ordered]@{ path = $precreated; pattern = '*inherits an external DACL*' },
            [ordered]@{ path = $unowned; pattern = '*owner is not SYSTEM or Administrators*' },
            [ordered]@{ path = $junctionRoot; pattern = '*traverses a reparse point*' }
        )) {
        Assert-Rejected {
            & $InstallerPath -PackagePath $dummyPackage -PackageSha256 $dummyPackageSha `
                -ModelArtifactPath $dummyModel -ApiKeyFile $dummyKey `
                -StateRoot ([string]$fixture.path) -NoStart | Out-Null
        } ([string]$fixture.pattern) 'installer crossed an unsafe state root before package or secret validation'
    }

    $childRoot = Initialize-NInferProtectedStateRoot (Join-Path $testRoot 'child-root')
    $childTarget = Join-Path $testRoot 'child-target'
    $childJunction = Join-Path $childRoot 'redirected-child'
    New-Item -ItemType Directory -Path $childTarget | Out-Null
    & cmd.exe /d /c "mklink /J `"$childJunction`" `"$childTarget`"" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'failed to create the child-junction fixture' }
    Assert-Rejected { Assert-NInferProtectedStateTree $childRoot } `
        '*contains a reparse point*' 'junction child was accepted'

    $requestLogRoot = Initialize-NInferProtectedStateRoot (Join-Path $testRoot 'request-log-root')
    $requestLog = Join-Path $requestLogRoot 'releases\fixture\logs\requests.jsonl'
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $requestLog) | Out-Null
    [IO.File]::WriteAllText($requestLog, "{}`n", [Text.UTF8Encoding]::new($false))
    Assert-NInferProtectedStateTree $requestLogRoot
    Assert-True ([IO.Path]::GetFullPath($requestLog).StartsWith(
            [IO.Path]::GetFullPath($requestLogRoot) + [IO.Path]::DirectorySeparatorChar,
            [StringComparison]::OrdinalIgnoreCase
        )) 'request JSONL did not resolve below protected state'
    $requestLogDenials = Get-LowPrivilegeFileDenials $requestLog $user $password
    Assert-True ([bool]$requestLogDenials.read_denied -and [bool]$requestLogDenials.write_denied) `
        'protected request JSONL allowed low-privilege read or write access'

    $managedReleasesVerified = 0
    $managedRequestLogsVerified = 0
    $managedRollbackDirections = 0
    $managedStatusMilliseconds = 0
    if (-not [string]::IsNullOrWhiteSpace($ManagedStateRoot)) {
        if (-not (Test-Path -LiteralPath $ManagedStateRoot -PathType Container)) {
            throw 'managed upgrade state root is missing'
        }
        $managedRoot = (Resolve-Path -LiteralPath $ManagedStateRoot).Path
        Assert-NInferProtectedStateTree $managedRoot
        $managedController = Join-Path $managedRoot 'Control-Release.ps1'
        if (-not (Test-Path -LiteralPath $managedController -PathType Leaf)) {
            throw 'managed release controller is missing'
        }

        $getManagedState = {
            Get-Content -LiteralPath (Join-Path $managedRoot 'state.json') -Raw | ConvertFrom-Json
        }
        $assertManagedSecrets = {
            param([object]$State)
            $hashes = [ordered]@{}
            foreach ($releaseProperty in @($State.releases.PSObject.Properties)) {
                $releaseId = [string]$releaseProperty.Name
                $release = $releaseProperty.Value
                $expectedDirectory = Join-Path (Join-Path $managedRoot 'secrets') $releaseId
                $expectedKey = Join-Path $expectedDirectory 'api-key.txt'
                Assert-True ([string]::Equals(
                        [IO.Path]::GetFullPath([string]$release.api_key_file),
                        [IO.Path]::GetFullPath($expectedKey),
                        [StringComparison]::OrdinalIgnoreCase
                    )) "retained release secret path mismatch: $releaseId"
                Assert-NInferProtectedAcl $expectedDirectory $true
                Assert-NInferProtectedAcl $expectedKey $true
                $denials = Get-LowPrivilegeFileDenials $expectedKey $user $password
                Assert-True ([bool]$denials.read_denied -and [bool]$denials.write_denied) `
                    "retained release secret allowed low-privilege access: $releaseId"
                $hashes[$releaseId] = (Get-FileHash -Algorithm SHA256 -LiteralPath $expectedKey).Hash.ToLowerInvariant()

                $requestPath = Join-Path ([string]$release.release_root) 'logs\requests.jsonl'
                if (Test-Path -LiteralPath $requestPath -PathType Leaf) {
                    Assert-NInferProtectedAcl $requestPath
                    $requestDenials = Get-LowPrivilegeFileDenials $requestPath $user $password
                    Assert-True ([bool]$requestDenials.read_denied -and [bool]$requestDenials.write_denied) `
                        "managed request log allowed low-privilege access: $releaseId"
                    $script:managedRequestLogsVerified++
                }
                $script:managedReleasesVerified++
            }
            return $hashes
        }

        $initialManagedState = & $getManagedState
        Assert-True (-not [string]::IsNullOrWhiteSpace([string]$initialManagedState.active_release) -and
            -not [string]::IsNullOrWhiteSpace([string]$initialManagedState.previous_release) -and
            [string]$initialManagedState.active_release -cne [string]$initialManagedState.previous_release) `
            'managed state does not contain a completed upgrade with a retained previous release'
        $initialActive = [string]$initialManagedState.active_release
        $initialPrevious = [string]$initialManagedState.previous_release
        $managedStateHelperPath = Join-Path $managedRoot 'Protect-StateRoot.ps1'
        $managedOwnerHelperPath = Join-Path (Join-Path $managedRoot 'gpu-owner') 'Protect-StateRoot.ps1'
        $initialStateHelperSha = (Get-FileHash -Algorithm SHA256 -LiteralPath $managedStateHelperPath).Hash.ToLowerInvariant()
        $initialOwnerHelperSha = (Get-FileHash -Algorithm SHA256 -LiteralPath $managedOwnerHelperPath).Hash.ToLowerInvariant()
        $secretHashes = & $assertManagedSecrets $initialManagedState
        $statusTimer = [Diagnostics.Stopwatch]::StartNew()
        & $managedController -Action Status -StateRoot $managedRoot | Out-Null
        $statusTimer.Stop()
        $managedStatusMilliseconds = [int][Math]::Ceiling($statusTimer.Elapsed.TotalMilliseconds)
        Assert-True ($managedStatusMilliseconds -le 5000) `
            "populated protected-root status exceeded 5 seconds: $managedStatusMilliseconds ms"

        if ($ExerciseManagedRollback) {
            & $managedController -Action Rollback -StateRoot $managedRoot | Out-Null
            $rolledState = & $getManagedState
            Assert-True ([string]$rolledState.active_release -ceq $initialPrevious -and
                [string]$rolledState.previous_release -ceq $initialActive) `
                'managed rollback did not atomically swap active and previous releases'
            $rolledHashes = & $assertManagedSecrets $rolledState
            Assert-True ((Get-FileHash -Algorithm SHA256 -LiteralPath $managedStateHelperPath).Hash.ToLowerInvariant() -ceq $initialStateHelperSha) 'managed rollback changed root state-helper bytes'
            Assert-True ((Get-FileHash -Algorithm SHA256 -LiteralPath $managedOwnerHelperPath).Hash.ToLowerInvariant() -ceq $initialOwnerHelperSha) 'managed rollback changed GPU-owner state-helper bytes'
            foreach ($releaseId in $secretHashes.Keys) {
                Assert-True ([string]$rolledHashes[$releaseId] -ceq [string]$secretHashes[$releaseId]) `
                    "managed rollback changed a retained release secret: $releaseId"
            }
            $managedRollbackDirections++

            & $managedController -Action Rollback -StateRoot $managedRoot | Out-Null
            $restoredManagedState = & $getManagedState
            Assert-True ([string]$restoredManagedState.active_release -ceq $initialActive -and
                [string]$restoredManagedState.previous_release -ceq $initialPrevious) `
                'reverse managed rollback did not restore the upgraded release'
            $restoredHashes = & $assertManagedSecrets $restoredManagedState
            Assert-True ((Get-FileHash -Algorithm SHA256 -LiteralPath $managedStateHelperPath).Hash.ToLowerInvariant() -ceq $initialStateHelperSha) 'reverse rollback changed root state-helper bytes'
            Assert-True ((Get-FileHash -Algorithm SHA256 -LiteralPath $managedOwnerHelperPath).Hash.ToLowerInvariant() -ceq $initialOwnerHelperSha) 'reverse rollback changed GPU-owner state-helper bytes'
            foreach ($releaseId in $secretHashes.Keys) {
                Assert-True ([string]$restoredHashes[$releaseId] -ceq [string]$secretHashes[$releaseId]) `
                    "reverse rollback changed a retained release secret: $releaseId"
            }
            $managedRollbackDirections++
            & $managedController -Action Stop -StateRoot $managedRoot | Out-Null
            $managedOwner = $restoredManagedState.gpu_owner
            & ([string]$managedOwner.controller_path) -Action start `
                -StateRoot ([string]$managedOwner.state_root) | Out-Null
        }
    }

    if ($InstrumentGpuPowerFixture) {
        $gpuOwnerSource = (Get-Content -LiteralPath $GpuOwnerControllerPath -Raw -Encoding UTF8).Replace(
            [string]([char]13 + [char]10), [string][char]10
        )
        $trustedResolver = @'
$nvidiaSmi = Join-Path ([Environment]::GetFolderPath('System')) 'nvidia-smi.exe'
if (-not (Test-Path -LiteralPath $nvidiaSmi -PathType Leaf)) {
    $nvidiaSmi = Join-Path $env:ProgramFiles 'NVIDIA Corporation\NVSMI\nvidia-smi.exe'
}
if (-not (Test-Path -LiteralPath $nvidiaSmi -PathType Leaf)) {
    throw 'trusted nvidia-smi installation is missing'
}
'@
        $fixtureResolver = '$nvidiaSmi = ''nvidia-smi.exe'''
        if ([regex]::Matches($gpuOwnerSource, [regex]::Escape($trustedResolver)).Count -ne 1) {
            throw 'GPU-owner trusted resolver fixture anchor changed'
        }
        $gpuOwnerSource = $gpuOwnerSource.Replace($trustedResolver, $fixtureResolver)
        $instrumentedGpuOwner = Join-Path $testRoot 'Control-GpuOwner.instrumented.ps1'
        [IO.File]::WriteAllText($instrumentedGpuOwner, $gpuOwnerSource, [Text.UTF8Encoding]::new($false))
        $GpuOwnerControllerPath = $instrumentedGpuOwner
    }

    $global:NInferTestPowerLimitW = 370
    $global:NInferTestInteractiveGpuActive = $false
    $global:NInferNvidiaShimCalls = 0
    function global:nvidia-smi.exe {
        [CmdletBinding()]
        param([Parameter(ValueFromRemainingArguments = $true)][object[]]$Remaining)
        $global:NInferNvidiaShimCalls++
        $values = @($Remaining | ForEach-Object { [string]$_ })
        if ($values -contains '--query-gpu=power.limit') {
            ([double]$global:NInferTestPowerLimitW).ToString(
                '0.00', [Globalization.CultureInfo]::InvariantCulture
            )
        }
        elseif ($values.Count -ge 2 -and $values[0] -ceq '-pl') {
            $global:NInferTestPowerLimitW = [int]$values[1]
        }
        Set-Variable -Name LASTEXITCODE -Value 0 -Scope 1
    }
    $absoluteNvidiaSmi = Join-Path ([Environment]::GetFolderPath('System')) 'nvidia-smi.exe'
    if (-not (Test-Path -LiteralPath $absoluteNvidiaSmi -PathType Leaf)) {
        $absoluteNvidiaSmi = Join-Path $env:ProgramFiles 'NVIDIA Corporation\NVSMI\nvidia-smi.exe'
    }
    $absolutePower = ((& $absoluteNvidiaSmi --query-gpu=power.limit --format=csv,noheader,nounits) | Out-String).Trim()
    Assert-True ($LASTEXITCODE -eq 0 -and $absolutePower -match '^[0-9]+(?:[.][0-9]+)?$') 'trusted absolute nvidia-smi query failed'
    Assert-True ($global:NInferNvidiaShimCalls -eq 0) 'path-qualified nvidia-smi unexpectedly invoked the function shim'
    $absoluteNvidiaShimInterceptions = $global:NInferNvidiaShimCalls
    function global:Get-CimInstance {
        param([string]$ClassName, [object]$ErrorAction)
        if ($global:NInferTestInteractiveGpuActive) {
            return [pscustomobject]@{ Name = 'pid_4242_luid_0x00000000_0x00000000_phys_0'; DedicatedUsage = 2GB }
        }
        return @()
    }

    $gpuRoot = Join-Path $testRoot 'gpu-owner'
    $gpuStatus = (((& $GpuOwnerControllerPath -Action status -StateRoot $gpuRoot) | Out-String).Trim() | ConvertFrom-Json)
    Assert-True ([int]$gpuStatus.power_limit_w -eq 370 -and -not [bool]$gpuStatus.paused) 'initial GPU-owner status envelope mismatch'
    & $GpuOwnerControllerPath -Action stop -StateRoot $gpuRoot | Out-Null
    $pausedStatus = (((& $GpuOwnerControllerPath -Action status -StateRoot $gpuRoot) | Out-String).Trim() | ConvertFrom-Json)
    Assert-True ([bool]$pausedStatus.paused -and [int]$pausedStatus.power_limit_w -eq 300) 'GPU owner did not enter the qualified cap'
    & $GpuOwnerControllerPath -Action stop -StateRoot $gpuRoot | Out-Null
    $idempotentPausedStatus = (((& $GpuOwnerControllerPath -Action status -StateRoot $gpuRoot) | Out-String).Trim() | ConvertFrom-Json)
    Assert-True ([bool]$idempotentPausedStatus.paused -and [int]$idempotentPausedStatus.power_limit_w -eq 300) 'non-default GPU-owner idempotent stop changed roots or power state'
    Assert-NInferProtectedStateTree $gpuRoot

    $global:NInferTestInteractiveGpuActive = $true
    $activeRejected = $false
    try { & $GpuOwnerControllerPath -Action stop -StateRoot $gpuRoot | Out-Null }
    catch { $activeRejected = $_.Exception.Message -like '*active interactive workload*' }
    Assert-True $activeRejected 'state-present lease acquisition did not re-check active interactive GPU work'
    $global:NInferTestInteractiveGpuActive = $false
    & $GpuOwnerControllerPath -Action start -StateRoot $gpuRoot | Out-Null
    $restoredStatus = (((& $GpuOwnerControllerPath -Action status -StateRoot $gpuRoot) | Out-String).Trim() | ConvertFrom-Json)
    Assert-True (-not [bool]$restoredStatus.paused -and [int]$restoredStatus.power_limit_w -eq 370) 'GPU owner did not restore the prior power limit'

    $preplantedGpu = Join-Path $testRoot 'preplanted-gpu-owner'
    New-Item -ItemType Directory -Path $preplantedGpu | Out-Null
    [IO.File]::WriteAllText(
        (Join-Path $preplantedGpu 'state.json'),
        '{"artifact_type":"ninfer_generic_gpu_owner_state","schema_version":1,"mode":"idle-host","paused":true}',
        [Text.UTF8Encoding]::new($false)
    )
    Assert-Rejected {
        & $GpuOwnerControllerPath -Action status -StateRoot $preplantedGpu | Out-Null
    } '*protected state inherits an external DACL*' 'preplanted generic GPU-owner state was accepted'

    $releaseScriptRoot = Split-Path -Parent $InstallerPath
    $releaseScripts = [Collections.Generic.List[IO.FileInfo]]::new()
    foreach ($scriptFile in @(Get-ChildItem -LiteralPath $releaseScriptRoot -File | Where-Object {
            $_.Name -in @(
                'Install-Release.ps1', 'Control-Release.ps1', 'Control-GpuOwner.ps1',
                'Protect-StateRoot.ps1', 'New-Package.ps1', 'New-QualificationReceipt.ps1'
            )
        })) { $releaseScripts.Add($scriptFile) }
    $installedConstructor = Join-Path (Join-Path (Split-Path -Parent $releaseScriptRoot) 'qualification') 'New-QualificationReceipt.ps1'
    if (Test-Path -LiteralPath $installedConstructor -PathType Leaf) {
        $releaseScripts.Add((Get-Item -LiteralPath $installedConstructor))
    }
    Assert-True ($releaseScripts.Count -ge 5) 'published release script inventory is incomplete'
    $forbiddenHookPatterns = @(
        'InstallTestMode', 'Invoke-InstallFault', 'NINFER_TEST_INSTALL_',
        'NInferSimulatedInterruption', 'NInferLifecycleHarness',
        'InternalSourceTestMode', 'TestBypass', 'FaultInjection',
        'SimulatedFailure', 'SimulatedInterruption'
    )
    foreach ($scriptFile in $releaseScripts) {
        $scriptSource = Get-Content -LiteralPath $scriptFile.FullName -Raw -Encoding UTF8
        foreach ($forbidden in $forbiddenHookPatterns) {
            Assert-True (-not $scriptSource.Contains($forbidden)) `
                "published release script contains a test bypass or fault hook: $($scriptFile.Name):$forbidden"
        }
    }

    $receipt = [ordered]@{
        artifact_type = 'ninfer_windows_state_security_regression'
        schema_version = 2
        status = 'passed'
        root_owner_sid = $rootOwner
        root_dacl_protected = $true
        clean_default_managed_parent_creations = 2
        atomic_race_collision_rejections = 1
        raced_state_recursive_deletions = 0
        null_dacl_rejections = 1
        low_privilege_effective_read_denials = 2
        low_privilege_effective_write_denials = 2
        precreated_root_rejections = 1
        unowned_root_rejections = 1
        root_junction_rejections = 1
        child_junction_rejections = 1
        preplanted_gpu_owner_rejections = 1
        installer_prewrite_root_rejections = 3
        active_interactive_gpu_rejections = 1
        qualified_power_limit_w = 300
        restored_power_limit_w = 370
        gpu_power_evidence_class = $(if ($InstrumentGpuPowerFixture) { 'instrumented-function-shim-no-hardware-claim' } else { 'real-trusted-absolute-nvidia-smi' })
        absolute_nvidia_shim_interceptions = $absoluteNvidiaShimInterceptions
        gpu_power_fixture_calls = $(if ($InstrumentGpuPowerFixture) { $global:NInferNvidiaShimCalls } else { 0 })
        request_jsonl_under_protected_state = $true
        request_jsonl_effective_access_denials = 2
        managed_release_acl_state_assertions = $managedReleasesVerified
        managed_request_log_acl_state_assertions = $managedRequestLogsVerified
        managed_rollback_directions = $managedRollbackDirections
        retained_state_helper_hash_assertions = $(if ($managedRollbackDirections -eq 2) { 4 } else { 0 })
        populated_root_status_milliseconds = $managedStatusMilliseconds
        shipped_test_bypass = $false
        shipped_scripts_scanned = $releaseScripts.Count
        forbidden_hook_patterns_scanned = $forbiddenHookPatterns.Count
    }
    if (-not [string]::IsNullOrWhiteSpace($ReceiptPath)) { Write-JsonAtomic $ReceiptPath $receipt }
    $receipt | ConvertTo-Json -Compress
}
finally {
    if ($null -ne $gpuRoot -and (Test-Path -LiteralPath (Join-Path $gpuRoot 'lease.json') -PathType Leaf)) {
        try { & $GpuOwnerControllerPath -Action start -StateRoot $gpuRoot | Out-Null }
        catch { Write-Error "failed to restore GPU power limit from retained lease evidence: $($_.Exception.Message)" }
    }
    $env:ProgramData = $originalProgramData
    if ($userCreated) { & net.exe user $user '/delete' | Out-Null }
    Remove-Item -LiteralPath $testRoot -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath Function:\nvidia-smi.exe -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath Function:\Get-CimInstance -Force -ErrorAction SilentlyContinue
}
