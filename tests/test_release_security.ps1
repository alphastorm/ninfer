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

    [switch]$RequireActiveCompute,

    [string]$ReceiptPath
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

$originalProgramData = $env:ProgramData
$testRoot = Join-Path $originalProgramData ('NInferSecurityRegression-' + [Guid]::NewGuid().ToString('N'))
$user = 'NfAcl' + [Guid]::NewGuid().ToString('N').Substring(0, 8)
$password = 'Nf!9aA' + [Guid]::NewGuid().ToString('N').Substring(0, 14)
$userCreated = $false
try {
    $testRoot = Initialize-NInferProtectedStateRoot $testRoot
    $env:ProgramData = $testRoot
    $cleanDefaultParent = Join-Path $env:ProgramData 'NInfer'
    $cleanDefaultRoot = Join-Path $cleanDefaultParent 'qwen38-4090'
    Assert-True (-not (Test-Path -LiteralPath $cleanDefaultParent)) 'clean default managed parent fixture already exists'
    $cleanDefaultRoot = Initialize-NInferProtectedStateRoot $cleanDefaultRoot
    Assert-NInferProtectedAcl $cleanDefaultParent $true
    Assert-NInferProtectedStateTree $cleanDefaultRoot
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
    $token = [IntPtr]::Zero
    if (-not [NInferSecurityNative]::LogonUser($user, $env:COMPUTERNAME, $password, 2, 0, [ref]$token)) {
        throw "failed to log on the low-privilege effective-access principal: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
    }
    $lowPrivilegeDenied = $false
    $impersonation = $null
    try {
        $impersonation = [Security.Principal.WindowsIdentity]::Impersonate($token)
        try {
            [IO.File]::WriteAllText(
                (Join-Path $protected 'low-privilege-write.txt'),
                'forbidden',
                [Text.UTF8Encoding]::new($false)
            )
        }
        catch [UnauthorizedAccessException] { $lowPrivilegeDenied = $true }
    }
    finally {
        if ($null -ne $impersonation) { $impersonation.Undo(); $impersonation.Dispose() }
        [NInferSecurityNative]::CloseHandle($token) | Out-Null
    }
    Assert-True $lowPrivilegeDenied 'protected root allowed a real low-privilege write'

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

    $gpuRoot = Join-Path $testRoot 'gpu-owner'
    $gpuStatus = (((& $GpuOwnerControllerPath -Action status -StateRoot $gpuRoot) | Out-String).Trim() | ConvertFrom-Json)
    Assert-True ([string]$gpuStatus.artifact_type -ceq 'ninfer_generic_gpu_owner_status') 'real GPU-owner status envelope mismatch'
    Assert-NInferProtectedStateTree $gpuRoot
    $activeComputeRejected = $false
    if ([int]$gpuStatus.active_compute_owner_count -gt 0) {
        try { & $GpuOwnerControllerPath -Action stop -StateRoot $gpuRoot | Out-Null }
        catch { $activeComputeRejected = $_.Exception.Message -like '*unmanaged GPU compute owner is active*' }
        Assert-True $activeComputeRejected 'real active compute owner was not rejected'
    }
    elseif ($RequireActiveCompute) {
        throw 'real active-compute fixture was required but nvidia-smi reported no compute owner'
    }

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

    $receipt = [ordered]@{
        artifact_type = 'ninfer_windows_state_security_regression'
        schema_version = 1
        status = 'passed'
        root_owner_sid = $rootOwner
        root_dacl_protected = $true
        clean_default_managed_parent_creations = 2
        low_privilege_effective_write_denials = 1
        precreated_root_rejections = 1
        unowned_root_rejections = 1
        root_junction_rejections = 1
        child_junction_rejections = 1
        preplanted_gpu_owner_rejections = 1
        installer_prewrite_root_rejections = 3
        active_compute_owner_observed = [int]$gpuStatus.active_compute_owner_count -gt 0
        active_compute_owner_rejections = if ($activeComputeRejected) { 1 } else { 0 }
        gpu_owner_status_rejected_active_service = $false
        request_jsonl_under_protected_state = $true
        shipped_test_bypass = $false
    }
    if (-not [string]::IsNullOrWhiteSpace($ReceiptPath)) { Write-JsonAtomic $ReceiptPath $receipt }
    $receipt | ConvertTo-Json -Compress
}
finally {
    $env:ProgramData = $originalProgramData
    if ($userCreated) { & net.exe user $user '/delete' | Out-Null }
    Remove-Item -LiteralPath $testRoot -Recurse -Force -ErrorAction SilentlyContinue
}
