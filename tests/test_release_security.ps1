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

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^GPU-[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}$')]
    [string]$GpuUuid,

    [switch]$ExerciseManagedRollback,

    [switch]$InstrumentGpuPowerFixture,

    # The lane's GPU-owner power policy: the managed cap while the release runs (0 for a lane
    # that leaves the limit alone) and the owner limit the fixture starts from.
    [ValidateRange(0, 1000)]
    [int]$QualifiedPowerLimitW = 300,

    [ValidateRange(1, 1000)]
    [int]$OwnerPowerLimitW = 370
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
$realGpuOwnerControllerPath = $GpuOwnerControllerPath
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
    $cleanDefaultRoot = Join-Path $cleanDefaultParent 'qwen38-native-fixture'
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

    # A running server renames checkpoint staging directories into generations while a controller
    # walks the state tree; v0.8.7's first qualification failed its rollback phase on one. This
    # Get-ChildItem shadows the cmdlet for the walks and removes the staging directory right after
    # its parent is listed for the (skip+1)th time, the order that race produced.
    $walkRoot = Initialize-NInferProtectedStateRoot (Join-Path $testRoot 'walk-root')
    $walkGenerations = Join-Path $walkRoot 'cache\session-checkpoints-v3\sessions\fixture\generations'
    $walkStaging = Join-Path $walkGenerations '.staging-1'
    function Get-ChildItem([string]$LiteralPath, [switch]$Force, [switch]$Recurse) {
        $listed = @(Microsoft.PowerShell.Management\Get-ChildItem -LiteralPath $LiteralPath -Force:$Force -Recurse:$Recurse)
        if ($null -ne $script:vanish -and $LiteralPath -ceq $walkGenerations) {
            if ($script:vanish.skip -gt 0) { $script:vanish.skip -= 1 }
            else {
                Remove-Item -LiteralPath $walkStaging -Recurse -Force
                $script:vanish = $null
            }
        }
        $listed
    }
    try {
        foreach ($case in @(
                [ordered]@{ name = 'controller state-root walk'; skip = 0; walk = { Initialize-NInferProtectedStateRoot $walkRoot | Out-Null } },
                [ordered]@{ name = 'protected ACL walk'; skip = 1; walk = { Assert-NInferProtectedStateTree $walkRoot } }
            )) {
            New-Item -ItemType Directory -Force -Path (Join-Path $walkStaging 'engine') | Out-Null
            $script:vanish = @{ skip = [int]$case.skip }
            & $case.walk
            Assert-True ($null -eq $script:vanish -and -not (Test-Path -LiteralPath $walkStaging)) `
                "$($case.name): the staging directory did not vanish during the walk"
        }
    }
    finally {
        $script:vanish = $null
        Remove-Item -LiteralPath Function:\Get-ChildItem
    }
    Assert-Rejected { Assert-NInferNoReparseTree (Join-Path $testRoot 'no-such-walk-root') } `
        '*does not exist*' 'a missing walk root was accepted'

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
        $initialActive = [string]$initialManagedState.active_release
        $initialPrevious = [string]$initialManagedState.previous_release
        if ([string]::IsNullOrWhiteSpace($initialPrevious)) {
            # The first release of a lineage (a lane's first install on a host) retains no predecessor, so there is
            # nothing to roll back to; its one release's protections are still checked below.
            Assert-True (-not [string]::IsNullOrWhiteSpace($initialActive) -and
                @($initialManagedState.releases.PSObject.Properties).Count -eq 1 -and
                $null -ne $initialManagedState.releases.PSObject.Properties[$initialActive] -and
                -not $ExerciseManagedRollback) `
                'managed state without a previous release must hold only its first release, which cannot be rolled back'
        }
        else {
            Assert-True (-not [string]::IsNullOrWhiteSpace($initialActive) -and $initialActive -cne $initialPrevious) `
                'managed state does not contain a completed upgrade with a retained previous release'
        }
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
                -StateRoot ([string]$managedOwner.state_root) -GpuUuid $GpuUuid -QualifiedPowerLimitW $QualifiedPowerLimitW | Out-Null
        }
    }

    # Use the shipped mapper, not a second P/Invoke definition, to retain real LUID attribution
    # while the workload fixture changes only dedicated usage on the selected adapter.
    $ownerAst = [Management.Automation.Language.Parser]::ParseFile($GpuOwnerControllerPath, [ref]$null, [ref]$null)
    $mapper = $ownerAst.Find({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq 'Get-AdapterPciAddress' }, $true)
    if ($null -eq $mapper) { throw 'shipped GPU-owner LUID mapper is missing' }
    . ([scriptblock]::Create($mapper.Extent.Text))
    $absoluteNvidiaSmi = Join-Path ([Environment]::GetFolderPath('System')) 'nvidia-smi.exe'
    if (-not (Test-Path -LiteralPath $absoluteNvidiaSmi -PathType Leaf)) {
        $absoluteNvidiaSmi = Join-Path $env:ProgramFiles 'NVIDIA Corporation\NVSMI\nvidia-smi.exe'
    }
    $pciRows = @(& $absoluteNvidiaSmi -i $GpuUuid --query-gpu=uuid,pci.bus_id --format=csv,noheader,nounits)
    Assert-True ($LASTEXITCODE -eq 0 -and $pciRows.Count -eq 1) 'selected GPU PCI identity unavailable'
    $global:NInferTestGpuIdentity = [string]$pciRows[0]
    $pciParts = @(([string]$pciRows[0] -split ',') | ForEach-Object { $_.Trim() })
    $GpuUuid = $pciParts[0]
    $gpuPciBusId = $pciParts[1].ToUpperInvariant()
    $selectedAddress = $pciParts[1].Substring(9).ToUpperInvariant()
    $global:NInferTestGpuCounterName = $null
    $global:NInferTestOtherCounterName = $null
    $global:NInferTestAdapterRows = @(Get-CimInstance -ClassName Win32_PerfFormattedData_GPUPerformanceCounters_GPUAdapterMemory -ErrorAction Stop)
    $global:NInferTestSelectedAdapterName = $null
    foreach ($row in $global:NInferTestAdapterRows) {
        if ([string]$row.Name -notmatch '^luid_0x([0-9a-f]{8})_0x([0-9a-f]{8})_phys_\d+$') { continue }
        $address = Get-AdapterPciAddress $Matches[1] $Matches[2]
        if ($address -ceq $selectedAddress) {
            $global:NInferTestSelectedAdapterName = [string]$row.Name
            $global:NInferTestGpuCounterName = 'pid_4242_' + [string]$row.Name
        }
        elseif ($address) { $global:NInferTestOtherCounterName = 'pid_4243_' + [string]$row.Name }
    }
    Assert-True ($null -ne $global:NInferTestGpuCounterName) 'cannot observe selected GPU adapter LUID for security regression'

    $gpuOwnerSource = (Get-Content -LiteralPath $GpuOwnerControllerPath -Raw -Encoding UTF8).Replace(
        [string]([char]13 + [char]10), [string][char]10
    )
    if ($InstrumentGpuPowerFixture) {
        $fixtureResolver = '$nvidiaSmi = ''nvidia-smi.exe'''
        $resolverStartMarker = '$nvidiaSmi = Join-Path ([Environment]::GetFolderPath(''System'')) ''nvidia-smi.exe'''
        $resolverEndMarker = "    throw 'trusted nvidia-smi installation is missing'`n}"
        $resolverStart = $gpuOwnerSource.IndexOf($resolverStartMarker, [StringComparison]::Ordinal)
        $resolverEnd = $gpuOwnerSource.IndexOf($resolverEndMarker, [StringComparison]::Ordinal)
        if ($resolverStart -lt 0 -or $resolverEnd -lt $resolverStart -or
            $gpuOwnerSource.LastIndexOf($resolverStartMarker, [StringComparison]::Ordinal) -ne $resolverStart) {
            throw 'GPU-owner trusted resolver fixture anchor changed'
        }
        $resolverEnd += $resolverEndMarker.Length
        $gpuOwnerSource = $gpuOwnerSource.Substring(0, $resolverStart) + $fixtureResolver +
            $gpuOwnerSource.Substring($resolverEnd)
    }
    # Only the temporary test copy substitutes resident-memory samples. Physical power
    # commands keep the trusted absolute executable unless the explicit power fixture is set.
    $tokens = $null; $errors = $null
    $ownerAst = [Management.Automation.Language.Parser]::ParseInput($gpuOwnerSource, [ref]$tokens, [ref]$errors)
    if ($errors.Count -ne 0) { throw 'GPU-owner fixture source does not parse' }
    $memoryFunction = $ownerAst.Find({
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq 'Get-GpuMemoryUsedBytes'
    }, $true)
    if ($null -eq $memoryFunction) { throw 'GPU-owner resident-memory function is missing' }
    $memoryCalls = @($memoryFunction.FindAll({
        param($node)
        $node -is [Management.Automation.Language.CommandAst] -and $node.GetCommandName() -ceq 'Invoke-NvidiaSmi'
    }, $true))
    if ($memoryCalls.Count -ne 1) { throw 'GPU-owner resident-memory query fixture anchor changed' }
    $memoryCommand = $memoryCalls[0].CommandElements[0].Extent
    $gpuOwnerSource = $gpuOwnerSource.Remove($memoryCommand.StartOffset, $memoryCommand.Text.Length).Insert(
        $memoryCommand.StartOffset, 'Invoke-NInferMemoryFixture'
    )
    $instrumentedGpuOwner = Join-Path $testRoot 'Control-GpuOwner.instrumented.ps1'
    [IO.File]::WriteAllText($instrumentedGpuOwner, $gpuOwnerSource, [Text.UTF8Encoding]::new($false))
    Copy-Item -LiteralPath $StateProtectionPath -Destination (Join-Path $testRoot 'Protect-StateRoot.ps1')
    $GpuOwnerControllerPath = $instrumentedGpuOwner

    $managedLimitW = if ($QualifiedPowerLimitW -gt 0) { $QualifiedPowerLimitW } else { $OwnerPowerLimitW }
    $ownerPolicy = @{ GpuUuid = $GpuUuid; QualifiedPowerLimitW = $QualifiedPowerLimitW; PriorPowerLimitMinW = 100; PriorPowerLimitMaxW = 1000 }
    $realStatus = & $realGpuOwnerControllerPath -Action status -StateRoot (Join-Path $testRoot 'observed-gpu-owner') @ownerPolicy | ConvertFrom-Json
    Assert-True ([int]$realStatus.interactive_gpu_workload_count -eq 0) 'close selected-card interactive workloads before the real power regression'
    $global:NInferTestPowerLimitW = $OwnerPowerLimitW
    $global:NInferTestMemoryUsedMiB = '186'
    $global:NInferNvidiaShimCalls = 0
    $global:NInferTestHideSelectedAdapter = $false
    $global:NInferTestCounterFailure = $false
    $preparedLeaseRestoreAssertions = 0
    function global:nvidia-smi.exe {
        [CmdletBinding()]
        param([Parameter(ValueFromRemainingArguments = $true)][object[]]$Remaining)
        $global:NInferNvidiaShimCalls++
        $values = @($Remaining | ForEach-Object { [string]$_ })
        $selector = [Array]::IndexOf($values, '-i')
        $powerOption = [Array]::IndexOf($values, '-pl')
        if ($selector -lt 0 -or $selector + 1 -ge $values.Count -or $values[$selector + 1] -cne $GpuUuid) { throw 'fixture received an unbound GPU command' }
        if ($values -contains '--query-gpu=power.limit') {
            ([double]$global:NInferTestPowerLimitW).ToString(
                '0.00', [Globalization.CultureInfo]::InvariantCulture
            )
        }
        elseif ($values -contains '--query-gpu=uuid,pci.bus_id') {
            $global:NInferTestGpuIdentity
        }
        elseif ($powerOption -ge 0 -and $powerOption + 1 -lt $values.Count) {
            $global:NInferTestPowerLimitW = [int]$values[$powerOption + 1]
        }
        Set-Variable -Name LASTEXITCODE -Value 0 -Scope 1
    }
    function global:Invoke-NInferMemoryFixture([string[]]$Arguments) {
        $selector = [Array]::IndexOf($Arguments, '-i')
        if ($selector -lt 0 -or $selector + 1 -ge $Arguments.Count -or
            $Arguments[$selector + 1] -cne $GpuUuid -or
            $Arguments -notcontains '--query-gpu=memory.used') {
            throw 'resident-memory fixture received an unbound or different measurement'
        }
        return $global:NInferTestMemoryUsedMiB
    }
    $absolutePower = ((& $absoluteNvidiaSmi -i $GpuUuid --query-gpu=power.limit --format=csv,noheader,nounits) | Out-String).Trim()
    Assert-True ($LASTEXITCODE -eq 0 -and $absolutePower -match '^[0-9]+(?:[.][0-9]+)?$') 'trusted absolute nvidia-smi query failed'
    Assert-True ($global:NInferNvidiaShimCalls -eq 0) 'path-qualified nvidia-smi unexpectedly invoked the function shim'
    $absoluteNvidiaShimInterceptions = $global:NInferNvidiaShimCalls
    function global:Get-CimInstance {
        param([string]$ClassName, [object]$ErrorAction)
        if ($global:NInferTestCounterFailure) { throw 'counter provider unavailable' }
        if ($ClassName -ceq 'Win32_PerfFormattedData_GPUPerformanceCounters_GPUAdapterMemory') {
            return @($global:NInferTestAdapterRows | Where-Object { -not $global:NInferTestHideSelectedAdapter -or [string]$_.Name -cne $global:NInferTestSelectedAdapterName })
        }
        if ($ClassName -cne 'Win32_PerfFormattedData_GPUPerformanceCounters_GPUProcessMemory') { throw 'unexpected counter class' }
        [pscustomobject]@{ Name = $global:NInferTestGpuCounterName; DedicatedUsage = 2GB }
        if ($global:NInferTestOtherCounterName) {
            [pscustomobject]@{ Name = $global:NInferTestOtherCounterName; DedicatedUsage = 3800MB }
        }
    }

    $gpuRoot = Join-Path $testRoot 'gpu-owner'
    $gpuStatus = (((& $GpuOwnerControllerPath -Action status -StateRoot $gpuRoot @ownerPolicy) | Out-String).Trim() | ConvertFrom-Json)
    Assert-True ([int]$gpuStatus.power_limit_w -eq $OwnerPowerLimitW -and -not [bool]$gpuStatus.paused) 'initial GPU-owner status envelope mismatch'
    Assert-True ([int]$gpuStatus.interactive_gpu_workload_count -eq 0) 'idle selected adapter was blocked by another card'
    Assert-True ([string]$gpuStatus.interactive_gpu_memory_source -ceq 'nvidia-smi.memory.used' -and
        [long]$gpuStatus.interactive_gpu_memory_used_bytes -eq 186MB -and
        [int]$gpuStatus.wddm_process_memory.above_threshold_process_count -eq 1) 'WDDM commit budget replaced selected-card residency'
    $global:NInferTestHideSelectedAdapter = $true
    $missingWddm = & $GpuOwnerControllerPath -Action status -StateRoot $gpuRoot @ownerPolicy | ConvertFrom-Json
    Assert-True ([string]$missingWddm.wddm_process_memory.status -ceq 'unavailable' -and
        [int]$missingWddm.interactive_gpu_workload_count -eq 0) 'missing WDDM diagnostics blocked valid selected-card residency'
    $global:NInferTestHideSelectedAdapter = $false
    $global:NInferTestCounterFailure = $true
    $failedWddm = & $GpuOwnerControllerPath -Action status -StateRoot $gpuRoot @ownerPolicy | ConvertFrom-Json
    Assert-True ([string]$failedWddm.wddm_process_memory.status -ceq 'unavailable' -and
        [int]$failedWddm.interactive_gpu_workload_count -eq 0) 'WDDM diagnostic failure replaced the residency gate'
    $global:NInferTestCounterFailure = $false
    $global:NInferTestMemoryUsedMiB = 'N/A'
    Assert-Rejected { & $GpuOwnerControllerPath -Action status -StateRoot $gpuRoot @ownerPolicy | Out-Null } '*cannot observe selected GPU resident memory*' 'unavailable residency was treated as idle'
    $global:NInferTestMemoryUsedMiB = '1023'
    $belowCeiling = & $GpuOwnerControllerPath -Action status -StateRoot $gpuRoot @ownerPolicy | ConvertFrom-Json
    Assert-True ([int]$belowCeiling.interactive_gpu_workload_count -eq 0) 'resident memory below 1 GiB was rejected'
    $global:NInferTestMemoryUsedMiB = '186'
    & $GpuOwnerControllerPath -Action stop -StateRoot $gpuRoot @ownerPolicy | Out-Null
    $pausedStatus = (((& $GpuOwnerControllerPath -Action status -StateRoot $gpuRoot @ownerPolicy) | Out-String).Trim() | ConvertFrom-Json)
    Assert-True ([bool]$pausedStatus.paused -and [int]$pausedStatus.power_limit_w -eq $managedLimitW) 'GPU owner did not enter the qualified cap'
    & $GpuOwnerControllerPath -Action stop -StateRoot $gpuRoot @ownerPolicy | Out-Null
    $idempotentPausedStatus = (((& $GpuOwnerControllerPath -Action status -StateRoot $gpuRoot @ownerPolicy) | Out-String).Trim() | ConvertFrom-Json)
    Assert-True ([bool]$idempotentPausedStatus.paused -and [int]$idempotentPausedStatus.power_limit_w -eq $managedLimitW) 'non-default GPU-owner idempotent stop changed roots or power state'
    Assert-NInferProtectedStateTree $gpuRoot
    $leasePath = Join-Path $gpuRoot 'lease.json'
    $leaseBefore = [IO.File]::ReadAllText($leasePath)
    try {
        $wrongLease = $leaseBefore | ConvertFrom-Json
        $wrongLease.gpu_pci_bus_id = '00000000:FF:1F.7'
        [IO.File]::WriteAllText($leasePath, ($wrongLease | ConvertTo-Json -Compress), [Text.UTF8Encoding]::new($false))
        Assert-Rejected { & $GpuOwnerControllerPath -Action start -StateRoot $gpuRoot @ownerPolicy | Out-Null } '*lease PCI identity*' 'wrong-card PCI lease was restored'
        Assert-True (Test-Path -LiteralPath $leasePath) 'rejected PCI lease was deleted'
        $wrongLease = $leaseBefore | ConvertFrom-Json
        $wrongLease.gpu_uuid = 'GPU-00000000-0000-0000-0000-000000000000'
        [IO.File]::WriteAllText($leasePath, ($wrongLease | ConvertTo-Json -Compress), [Text.UTF8Encoding]::new($false))
        Assert-Rejected { & $GpuOwnerControllerPath -Action start -StateRoot $gpuRoot @ownerPolicy | Out-Null } '*lease UUID*' 'wrong-card UUID lease was restored'
        Assert-True (Test-Path -LiteralPath $leasePath) 'rejected UUID lease was deleted'
    }
    finally { [IO.File]::WriteAllText($leasePath, $leaseBefore, [Text.UTF8Encoding]::new($false)) }

    $global:NInferTestMemoryUsedMiB = '1024'
    $activeRejected = $false
    try { & $GpuOwnerControllerPath -Action stop -StateRoot $gpuRoot @ownerPolicy | Out-Null }
    catch { $activeRejected = $_.Exception.Message -like '*active interactive workload*' }
    Assert-True $activeRejected 'state-present lease acquisition did not re-check active interactive GPU work'
    $global:NInferTestMemoryUsedMiB = '186'
    & $GpuOwnerControllerPath -Action start -StateRoot $gpuRoot @ownerPolicy | Out-Null
    $restoredStatus = (((& $GpuOwnerControllerPath -Action status -StateRoot $gpuRoot @ownerPolicy) | Out-String).Trim() | ConvertFrom-Json)
    Assert-True (-not [bool]$restoredStatus.paused -and [int]$restoredStatus.power_limit_w -eq $OwnerPowerLimitW) 'GPU owner did not restore the prior power limit'
    if ($InstrumentGpuPowerFixture -and $QualifiedPowerLimitW -gt 0) {
        [IO.File]::WriteAllText(
            (Join-Path $gpuRoot 'lease.json'),
            ('{"schema_version":1,"gpu_uuid":"' + $GpuUuid + '","gpu_pci_bus_id":"' + $gpuPciBusId + '","paused":true,"phase":"prepared","qualified_power_limit_w":' + $QualifiedPowerLimitW + ',"prior_power_limit_w":' + $OwnerPowerLimitW + '}'),
            [Text.UTF8Encoding]::new($false)
        )
        $global:NInferTestPowerLimitW = $QualifiedPowerLimitW
        & $GpuOwnerControllerPath -Action start -StateRoot $gpuRoot @ownerPolicy | Out-Null
        Assert-True ($global:NInferTestPowerLimitW -eq $OwnerPowerLimitW -and
            -not (Test-Path -LiteralPath (Join-Path $gpuRoot 'lease.json'))) 'prepared GPU-owner lease did not restore its prior limit'
        $preparedLeaseRestoreAssertions = 1
    }

    $preplantedGpu = Join-Path $testRoot 'preplanted-gpu-owner'
    New-Item -ItemType Directory -Path $preplantedGpu | Out-Null
    [IO.File]::WriteAllText(
        (Join-Path $preplantedGpu 'state.json'),
        '{"artifact_type":"ninfer_generic_gpu_owner_state","schema_version":1,"mode":"idle-host","paused":true}',
        [Text.UTF8Encoding]::new($false)
    )
    Assert-Rejected {
        & $GpuOwnerControllerPath -Action status -StateRoot $preplantedGpu @ownerPolicy | Out-Null
    } '*protected state inherits an external DACL*' 'preplanted generic GPU-owner state was accepted'

    $releaseScriptRoot = Split-Path -Parent $InstallerPath
    $controllerAst = [Management.Automation.Language.Parser]::ParseFile((Join-Path $releaseScriptRoot 'Control-Release.ps1'), [ref]$null, [ref]$null)
    $evidenceReader = $controllerAst.Find({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq 'Test-ServerStartGpu' }, $true)
    if ($null -eq $evidenceReader) { throw 'shipped startup GPU evidence reader is missing' }
    . ([scriptblock]::Create($evidenceReader.Extent.Text))
    $evidenceLog = Join-Path $testRoot 'startup-evidence.jsonl'
    $evidencePid = 4242
    $startup = [ordered]@{
        artifact_type = 'ninfer_serve_request_log'; schema_version = 30
        event = 'server_start'; server_instance_id = 'serve-4242-1'
        environment = @{ gpu_uuid = $GpuUuid; device = 0; gpu_name = 'Synthetic adapter' }
    }
    $historical = ($startup | ConvertTo-Json -Compress) + "`n"
    $encoding = [Text.UTF8Encoding]::new($false)
    [IO.File]::WriteAllText($evidenceLog, $historical, $encoding)
    $offset = $encoding.GetByteCount($historical)
    $creation = (Get-Item -LiteralPath $evidenceLog).CreationTimeUtc.Ticks
    Assert-True (-not (Test-ServerStartGpu $evidenceLog $GpuUuid $offset $creation $evidencePid 0 'Synthetic adapter' 30)) 'historical server_start was accepted for a new launch'
    $startup.server_instance_id = 'serve-4242-2'
    $fresh = $startup | ConvertTo-Json -Compress
    [IO.File]::AppendAllText($evidenceLog, $fresh, $encoding)
    Assert-True (-not (Test-ServerStartGpu $evidenceLog $GpuUuid $offset $creation $evidencePid 0 'Synthetic adapter' 30)) 'partial JSONL record established readiness'
    [IO.File]::AppendAllText($evidenceLog, "`n", $encoding)
    Assert-True (Test-ServerStartGpu $evidenceLog $GpuUuid $offset $creation $evidencePid 0 'Synthetic adapter' 30) 'fresh matching GPU startup was refused'
    $startup.environment.gpu_uuid = 'GPU-00000000-0000-0000-0000-000000000000'
    [IO.File]::WriteAllText($evidenceLog, ($historical + ($startup | ConvertTo-Json -Compress) + "`n"), $encoding)
    Assert-Rejected { Test-ServerStartGpu $evidenceLog $GpuUuid $offset $creation $evidencePid 0 'Synthetic adapter' 30 | Out-Null } '*GPU UUID differs*' 'fresh wrong-GPU startup was accepted'
    $startup.environment.gpu_uuid = $GpuUuid
    $startup.schema_version = 29
    [IO.File]::WriteAllText($evidenceLog, ($historical + ($startup | ConvertTo-Json -Compress) + "`n"), $encoding)
    Assert-Rejected { Test-ServerStartGpu $evidenceLog $GpuUuid $offset $creation $evidencePid 0 'Synthetic adapter' 30 | Out-Null } '*schema mismatch*' 'old request-log schema established readiness'
    $startup.schema_version = 30
    $startup.environment.device = 1
    [IO.File]::WriteAllText($evidenceLog, ($historical + ($startup | ConvertTo-Json -Compress) + "`n"), $encoding)
    Assert-Rejected { Test-ServerStartGpu $evidenceLog $GpuUuid $offset $creation $evidencePid 0 'Synthetic adapter' 30 | Out-Null } '*device or GPU name differs*' 'wrong CUDA ordinal established readiness'
    $startup.environment.device = 0
    $startup.environment.gpu_name = 'Another adapter'
    [IO.File]::WriteAllText($evidenceLog, ($historical + ($startup | ConvertTo-Json -Compress) + "`n"), $encoding)
    Assert-Rejected { Test-ServerStartGpu $evidenceLog $GpuUuid $offset $creation $evidencePid 0 'Synthetic adapter' 30 | Out-Null } '*device or GPU name differs*' 'wrong GPU name established readiness'
    $startup.environment.gpu_name = 'Synthetic adapter'
    $startup.server_instance_id = 'serve-4243-2'
    [IO.File]::WriteAllText($evidenceLog, ($historical + ($startup | ConvertTo-Json -Compress) + "`n"), $encoding)
    Assert-Rejected { Test-ServerStartGpu $evidenceLog $GpuUuid $offset $creation $evidencePid 0 'Synthetic adapter' 30 | Out-Null } '*does not belong*' 'foreign PID startup was accepted'
    [IO.File]::WriteAllText($evidenceLog, '', $encoding)
    Assert-Rejected { Test-ServerStartGpu $evidenceLog $GpuUuid $offset $creation $evidencePid 0 'Synthetic adapter' 30 | Out-Null } '*truncated*' 'truncated request log was accepted'
    Assert-Rejected { Test-ServerStartGpu $evidenceLog $GpuUuid 0 ($creation + 1) $evidencePid 0 'Synthetic adapter' 30 | Out-Null } '*replaced*' 'replaced request log was accepted'
    $releaseScripts = [Collections.Generic.List[IO.FileInfo]]::new()
    foreach ($scriptFile in @(Get-ChildItem -LiteralPath $releaseScriptRoot -File | Where-Object {
            $_.Name -in @(
                'Install-Release.ps1', 'Control-Release.ps1', 'Control-GpuOwner.ps1',
                'Protect-StateRoot.ps1', 'New-Package.ps1'
            )
        })) { $releaseScripts.Add($scriptFile) }
    Assert-True ($releaseScripts.Count -ge 4) 'published release script inventory is incomplete'
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
        qualified_power_limit_w = $QualifiedPowerLimitW
        restored_power_limit_w = $OwnerPowerLimitW
        gpu_power_evidence_class = $(if ($InstrumentGpuPowerFixture) { 'instrumented-function-shim-no-hardware-claim' } else { 'real-trusted-absolute-nvidia-smi' })
        absolute_nvidia_shim_interceptions = $absoluteNvidiaShimInterceptions
        gpu_power_fixture_calls = $(if ($InstrumentGpuPowerFixture) { $global:NInferNvidiaShimCalls } else { 0 })
        prepared_lease_restore_assertions = $preparedLeaseRestoreAssertions
        gpu_binding_lease_refusal_assertions = 2
        startup_gpu_evidence_assertions = 10
        gpu_workload_evidence_class = 'selected-nvml-residency-function-shim-and-real-idle-precheck'
        gpu_workload_measurement_source = 'nvidia-smi.memory.used'
        wddm_diagnostics_only_assertions = 2
        resident_memory_boundary_assertions = 2
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
        try { & $GpuOwnerControllerPath -Action start -StateRoot $gpuRoot @ownerPolicy | Out-Null }
        catch { Write-Error "failed to restore GPU power limit from retained lease evidence: $($_.Exception.Message)" }
    }
    $env:ProgramData = $originalProgramData
    if ($userCreated) { & net.exe user $user '/delete' | Out-Null }
    Remove-Item -LiteralPath $testRoot -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath Function:\nvidia-smi.exe -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath Function:\Invoke-NInferMemoryFixture -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath Function:\Get-CimInstance -Force -ErrorAction SilentlyContinue
}
