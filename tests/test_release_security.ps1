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

    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Container })]
    [string]$PublishedScriptRoot,

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
$PublishedScriptRoot = (Resolve-Path -LiteralPath $PublishedScriptRoot).Path
$stateProtectionText = [IO.File]::ReadAllText($StateProtectionPath, [Text.Encoding]::UTF8)
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
    [DllImport("advapi32.dll", CharSet=CharSet.Unicode)]
    private static extern uint SetNamedSecurityInfo(
        string objectName, int objectType, uint securityInfo,
        IntPtr owner, IntPtr group, IntPtr dacl, IntPtr sacl);
    public static uint SetNullFileDacl(string path) {
        const int FileObject = 1;
        const uint DaclSecurityInformation = 0x00000004;
        const uint ProtectedDaclSecurityInformation = 0x80000000;
        return SetNamedSecurityInfo(path, FileObject,
            DaclSecurityInformation | ProtectedDaclSecurityInformation,
            IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
    }
}
'@
}

$originalProgramData = $env:ProgramData
$testRoot = Join-Path $originalProgramData ('NInferSecurityRegression-' + [Guid]::NewGuid().ToString('N'))
$user = 'NfAcl' + [Guid]::NewGuid().ToString('N').Substring(0, 8)
$password = 'Nf!9aA' + [Guid]::NewGuid().ToString('N').Substring(0, 14)
$userCreated = $false
try {
    Assert-True $stateProtectionText.Contains('[NInferProtectedDirectoryNative]::Create') 'state helper does not use atomic ACL-bearing directory creation'
    Assert-True (-not $stateProtectionText.Contains('New-Item -ItemType Directory -Path $next')) 'state helper still creates a directory before applying its ACL'
    $publishedScripts = @(Get-ChildItem -LiteralPath $PublishedScriptRoot -File -Filter '*.ps1' | Sort-Object Name)
    $expectedPublishedScripts = @(
        'Compare-MtpQualification.ps1', 'Control-GpuOwner.ps1', 'Control-Release.ps1',
        'Install-Release.ps1', 'Invoke-Qualification.ps1', 'New-Package.ps1',
        'New-QualificationReceipt.ps1', 'Protect-StateRoot.ps1'
    ) | Sort-Object
    Assert-True (@(Compare-Object $expectedPublishedScripts @($publishedScripts.Name)).Count -eq 0) `
        'published PowerShell release class changed without hook review'
    $hookPattern = '(?i)(?:\b(?:TestMode|Mock|Fixture|Fault|Bypass|Harness)\b|\bInject(?:ed|ion)?\b|\bSimulat(?:e|ed|ion)\b|test[_-]?mode|mock[_-]|fixture[_-]|fault[_-]|inject[_-]|bypass[_-]|simulat[_-]|harness[_-]|NINFER_[A-Z0-9_]*(?:TEST|MOCK|FAULT|INJECT|BYPASS|SIMULAT|HARNESS))'
    foreach ($scriptPath in $publishedScripts) {
        $tokens = $null
        $errors = $null
        $ast = [Management.Automation.Language.Parser]::ParseFile(
            $scriptPath.FullName,
            [ref]$tokens,
            [ref]$errors
        )
        Assert-True (@($errors).Count -eq 0) "published script does not parse: $($scriptPath.Name)"
        $hookTokens = @($tokens | Where-Object {
                $_.Kind -cin @('Variable', 'SplattedVariable', 'Identifier', 'Generic') -and
                [string]$_.Text -cmatch $hookPattern
            })
        Assert-True ($hookTokens.Count -eq 0) "published script contains reachable hook vocabulary: $($scriptPath.Name)"
        $predicateText = [Collections.Generic.List[string]]::new()
        foreach ($node in $ast.FindAll({
                    param($candidate)
                    $candidate -is [Management.Automation.Language.IfStatementAst] -or
                    $candidate -is [Management.Automation.Language.WhileStatementAst] -or
                    $candidate -is [Management.Automation.Language.DoWhileStatementAst] -or
                    $candidate -is [Management.Automation.Language.DoUntilStatementAst] -or
                    $candidate -is [Management.Automation.Language.ForStatementAst] -or
                    $candidate -is [Management.Automation.Language.SwitchStatementAst] -or
                    $candidate -is [Management.Automation.Language.ParameterAst]
                }, $true)) {
            if ($node -is [Management.Automation.Language.IfStatementAst]) {
                foreach ($clause in $node.Clauses) { $predicateText.Add($clause.Item1.Extent.Text) }
            }
            elseif ($node -is [Management.Automation.Language.SwitchStatementAst]) {
                $predicateText.Add($node.Condition.Extent.Text)
                foreach ($clause in $node.Clauses) { $predicateText.Add($clause.Item1.Extent.Text) }
            }
            elseif ($node -is [Management.Automation.Language.ParameterAst]) {
                $predicateText.Add($node.Extent.Text)
            }
            elseif ($null -ne $node.Condition) { $predicateText.Add($node.Condition.Extent.Text) }
        }
        Assert-True (@($predicateText | Where-Object { $_ -cmatch $hookPattern }).Count -eq 0) `
            "published script contains literal-gated hook predicate: $($scriptPath.Name)"
        $bareNvidiaCommands = @($ast.FindAll({
                    param($candidate)
                    $candidate -is [Management.Automation.Language.CommandAst] -and
                    [string]$candidate.GetCommandName() -ieq 'nvidia-smi.exe'
                }, $true))
        Assert-True ($bareNvidiaCommands.Count -eq 0) `
            "published script invokes nvidia-smi through command lookup: $($scriptPath.Name)"
    }
    $syntheticTokens = $null
    $syntheticErrors = $null
    $syntheticAst = [Management.Automation.Language.Parser]::ParseInput(
        'if ($env:NINFER_TEST_MODE -eq ''fixture'') { Write-Output forbidden }',
        [ref]$syntheticTokens,
        [ref]$syntheticErrors
    )
    $syntheticIf = @($syntheticAst.FindAll({
                param($candidate)
                $candidate -is [Management.Automation.Language.IfStatementAst]
            }, $true))
    Assert-True ($syntheticErrors.Count -eq 0 -and $syntheticIf.Count -eq 1 -and
        $syntheticIf[0].Clauses[0].Item1.Extent.Text -cmatch $hookPattern) `
        'hook scan does not inspect string literals in executable predicates'
    foreach ($trustedName in @('Control-GpuOwner.ps1', 'Control-Release.ps1', 'Install-Release.ps1')) {
        $trustedText = [IO.File]::ReadAllText((Join-Path $PublishedScriptRoot $trustedName), [Text.Encoding]::UTF8)
        Assert-True ($trustedText.Contains('[Environment+SpecialFolder]::System') -and
            $trustedText.Contains('[Diagnostics.ProcessStartInfo]::new()')) `
            "published GPU query is not trusted-absolute and process-captured: $trustedName"
    }

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

    $secretDirectory = Join-Path $protected 'secrets\retained-release'
    New-Item -ItemType Directory -Path $secretDirectory -Force | Out-Null
    $adminProbe = Join-Path $secretDirectory 'api-key.txt'
    [IO.File]::WriteAllText($adminProbe, 'admin-only-secret', [Text.UTF8Encoding]::new($false))
    Assert-NInferProtectedStateTree $protected
    $nullDaclProbe = Join-Path $protected 'null-dacl.txt'
    [IO.File]::WriteAllText($nullDaclProbe, 'world-readable-only-if-null-dacl', [Text.UTF8Encoding]::new($false))
    $nullDaclError = [NInferSecurityNative]::SetNullFileDacl($nullDaclProbe)
    if ($nullDaclError -ne 0) { throw "failed to create NULL-DACL regression file: $nullDaclError" }
    Assert-Rejected { Assert-NInferProtectedStateTree $protected } '*NULL DACL*' 'protected state accepted a NULL-DACL descendant'
    $token = [IntPtr]::Zero
    if (-not [NInferSecurityNative]::LogonUser($user, $env:COMPUTERNAME, $password, 2, 0, [ref]$token)) {
        throw "failed to log on the low-privilege effective-access principal: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
    }
    $lowPrivilegeDenied = $false
    $lowPrivilegeReadDenied = $false
    $nullDaclReadAllowed = $false
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
        try { [IO.File]::ReadAllText($adminProbe) | Out-Null }
        catch [UnauthorizedAccessException] { $lowPrivilegeReadDenied = $true }
        try {
            [IO.File]::ReadAllText($nullDaclProbe) | Out-Null
            $nullDaclReadAllowed = $true
        }
        catch [UnauthorizedAccessException] { $nullDaclReadAllowed = $false }
    }
    finally {
        if ($null -ne $impersonation) { $impersonation.Undo(); $impersonation.Dispose() }
        [NInferSecurityNative]::CloseHandle($token) | Out-Null
    }
    Assert-True $lowPrivilegeDenied 'protected root allowed a real low-privilege write'
    Assert-True $lowPrivilegeReadDenied 'protected root allowed a real low-privilege read'
    Assert-True $nullDaclReadAllowed 'NULL-DACL regression file was not effectively world-readable'
    Remove-Item -LiteralPath $nullDaclProbe -Force
    Assert-NInferProtectedStateTree $protected

    $precreated = Join-Path $testRoot 'precreated'
    New-Item -ItemType Directory -Path $precreated | Out-Null
    $precreatedMarker = Join-Path $precreated 'attacker-marker.txt'
    [IO.File]::WriteAllText($precreatedMarker, 'must-survive', [Text.UTF8Encoding]::new($false))
    Assert-Rejected { Initialize-NInferProtectedStateRoot $precreated | Out-Null } `
        '*protected state inherits an external DACL*' 'attacker-precreated root was accepted'
    Assert-True (Test-Path -LiteralPath $precreatedMarker -PathType Leaf) 'state initialization recursively deleted an untrusted precreated tree'

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

    $defaultGpuRoot = Join-Path $originalProgramData 'NInfer\gpu-owner'
    $defaultGpuState = Join-Path $defaultGpuRoot 'state.json'
    $defaultGpuStateExisted = Test-Path -LiteralPath $defaultGpuState -PathType Leaf
    $defaultGpuStateSha256 = if ($defaultGpuStateExisted) {
        (Get-FileHash -Algorithm SHA256 -LiteralPath $defaultGpuState).Hash.ToLowerInvariant()
    }
    else { $null }
    $gpuRoot = Join-Path $testRoot 'nondefault-gpu-owner-state'
    $gpuStatus = (((& $GpuOwnerControllerPath -Action status -StateRoot $gpuRoot) | Out-String).Trim() | ConvertFrom-Json)
    Assert-True ([string]$gpuStatus.artifact_type -ceq 'ninfer_generic_gpu_owner_status') 'real GPU-owner status envelope mismatch'
    Assert-NInferProtectedStateTree $gpuRoot
    Assert-True ((Test-Path -LiteralPath $defaultGpuState -PathType Leaf) -eq $defaultGpuStateExisted) 'non-default GPU-owner invocation created or removed default state'
    if ($defaultGpuStateExisted) {
        Assert-True ((Get-FileHash -Algorithm SHA256 -LiteralPath $defaultGpuState).Hash.ToLowerInvariant() -ceq $defaultGpuStateSha256) 'non-default GPU-owner invocation changed default state'
    }
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
        evidence_class = 'exact-shipped-state-helper-and-installer-prewrite-real-acl'
        exact_shipped_installer_executed = $true
        exact_shipped_installer_full_install = $false
        root_owner_sid = $rootOwner
        root_dacl_protected = $true
        atomic_acl_directory_creation = $true
        untrusted_precreated_tree_preserved = $true
        clean_default_managed_parent_creations = 2
        low_privilege_effective_write_denials = 1
        low_privilege_effective_read_denials = 1
        bearer_secret_effective_read_denials = 1
        null_dacl_rejections = 1
        null_dacl_effective_read_observations = 1
        precreated_root_rejections = 1
        unowned_root_rejections = 1
        root_junction_rejections = 1
        child_junction_rejections = 1
        preplanted_gpu_owner_rejections = 1
        nondefault_gpu_owner_state_bindings = 1
        default_gpu_owner_state_mutations = 0
        installer_prewrite_root_rejections = 3
        active_compute_owner_observed = [int]$gpuStatus.active_compute_owner_count -gt 0
        active_compute_owner_rejections = if ($activeComputeRejected) { 1 } else { 0 }
        gpu_owner_status_rejected_active_service = $false
        nvidia_smi_evidence_class = 'exact-trusted-absolute-query'
        trusted_absolute_nvidia_smi_scripts = 3
        bare_nvidia_smi_command_invocations = 0
        gpu_power_limit_mutations = 0
        gpu_power_limit_restore_required = $false
        request_jsonl_under_protected_state = $true
        shipped_test_bypass = $false
        published_release_scripts_scanned = $publishedScripts.Count
        literal_predicate_hook_scan = $true
    }
    if (-not [string]::IsNullOrWhiteSpace($ReceiptPath)) { Write-JsonAtomic $ReceiptPath $receipt }
    $receipt | ConvertTo-Json -Compress
}
finally {
    $env:ProgramData = $originalProgramData
    if ($userCreated) { & net.exe user $user '/delete' | Out-Null }
    Remove-Item -LiteralPath $testRoot -Recurse -Force -ErrorAction SilentlyContinue
}
