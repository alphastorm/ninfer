[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$ControllerPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$publishedControllerPath = (Resolve-Path -LiteralPath $ControllerPath).Path
$principal = [Security.Principal.WindowsPrincipal]::new(
    [Security.Principal.WindowsIdentity]::GetCurrent()
)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { exit 77 }
$root = Join-Path $env:ProgramData ('NInferGpuOwnerRegression-' + [Guid]::NewGuid().ToString('N'))
$instrumentedDirectory = Join-Path $env:TEMP ('ninfer-fake-smi-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $instrumentedDirectory | Out-Null
$fakeNvidiaSmi = Join-Path $instrumentedDirectory 'nvidia-smi.exe'
Add-Type -TypeDefinition @'
using System;
public static class NInferFakeNvidiaSmi {
    public static int Main() {
        Console.Out.Write(Environment.GetEnvironmentVariable("NINFER_HARNESS_GPU_STDOUT") ?? "");
        Console.Error.Write(Environment.GetEnvironmentVariable("NINFER_HARNESS_GPU_STDERR") ?? "");
        int code;
        return Int32.TryParse(Environment.GetEnvironmentVariable("NINFER_HARNESS_GPU_EXIT"), out code)
            ? code : 0;
    }
}
'@ -OutputAssembly $fakeNvidiaSmi -OutputType ConsoleApplication

$publishedText = [IO.File]::ReadAllText($publishedControllerPath, [Text.Encoding]::UTF8)
$trustedLookup = @'
        $systemDirectory = [Environment]::GetFolderPath([Environment+SpecialFolder]::System)
        $nvidiaSmi = [IO.Path]::GetFullPath((Join-Path $systemDirectory 'nvidia-smi.exe'))
'@
$instrumentedLookup = "        `$nvidiaSmi = '$($fakeNvidiaSmi.Replace("'", "''"))'`r`n"
$lookupIndex = $publishedText.IndexOf($trustedLookup, [StringComparison]::Ordinal)
if ($lookupIndex -lt 0 -or
    $publishedText.IndexOf(
        $trustedLookup,
        $lookupIndex + $trustedLookup.Length,
        [StringComparison]::Ordinal
    ) -ge 0) {
    throw 'trusted nvidia-smi instrumentation anchor is missing or ambiguous'
}
$instrumentedText = $publishedText.Substring(0, $lookupIndex) + $instrumentedLookup +
    $publishedText.Substring($lookupIndex + $trustedLookup.Length)
$instrumentedControllerPath = Join-Path $instrumentedDirectory 'Control-GpuOwner.ps1'
[IO.File]::WriteAllText($instrumentedControllerPath, $instrumentedText, [Text.UTF8Encoding]::new($false))
$ControllerPath = $instrumentedControllerPath
Copy-Item -LiteralPath (Join-Path (Split-Path -Parent $publishedControllerPath) 'Protect-StateRoot.ps1') `
    -Destination (Join-Path $instrumentedDirectory 'Protect-StateRoot.ps1')

$originalStdout = $env:NINFER_HARNESS_GPU_STDOUT
$originalStderr = $env:NINFER_HARNESS_GPU_STDERR
$originalExit = $env:NINFER_HARNESS_GPU_EXIT
$env:NINFER_HARNESS_GPU_STDOUT = ''
$env:NINFER_HARNESS_GPU_STDERR = ''
$env:NINFER_HARNESS_GPU_EXIT = '0'

function Read-Status {
    return (((& $ControllerPath -Action status -StateRoot $root) | Out-String).Trim() | ConvertFrom-Json)
}
function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}
function Assert-StopRejected([string]$Message) {
    $rejected = $false
    try { & $ControllerPath -Action stop -StateRoot $root | Out-Null }
    catch { $rejected = $_.Exception.Message -like '*failed to inspect GPU compute owners*' -or
        $_.Exception.Message -like '*unmanaged GPU compute owner is active*' }
    Assert-True $rejected $Message
}

try {
    $initial = Read-Status
    Assert-True ([string]$initial.artifact_type -ceq 'ninfer_generic_gpu_owner_status') 'generic status envelope mismatch'
    Assert-True ([string]$initial.compute_owner_query_application -ceq 'trusted-system32-nvidia-smi') 'status omitted trusted query application classification'
    Assert-True ([bool]$initial.paused) 'clean empty query output did not produce idle paused state'
    & $ControllerPath -Action stop -StateRoot $root | Out-Null

    $env:NINFER_HARNESS_GPU_STDOUT = "4242`r`n"
    $activeStatus = Read-Status
    Assert-True (-not [bool]$activeStatus.paused) 'status reported an active compute owner as released'
    Assert-True ([int]$activeStatus.active_compute_owner_count -eq 1) "status lost the active compute-owner count: $($activeStatus | ConvertTo-Json -Compress)"
    Assert-StopRejected 'state-present stop did not re-check the active compute owner'

    $env:NINFER_HARNESS_GPU_STDOUT = "4242`r`nnot-a-pid`r`n"
    $malformed = Read-Status
    Assert-True (-not [bool]$malformed.compute_owner_query_available -and -not [bool]$malformed.paused) 'malformed row was filtered into an idle result'
    Assert-StopRejected 'malformed compute-owner row did not fail closed'

    $env:NINFER_HARNESS_GPU_STDOUT = ''
    $env:NINFER_HARNESS_GPU_STDERR = 'driver warning'
    $stderrStatus = Read-Status
    Assert-True (-not [bool]$stderrStatus.compute_owner_query_available) 'stderr-producing query was accepted'
    Assert-StopRejected 'stderr-producing query did not fail closed'

    $env:NINFER_HARNESS_GPU_STDERR = 'query failed'
    $env:NINFER_HARNESS_GPU_EXIT = '7'
    $exitStatus = Read-Status
    Assert-True (-not [bool]$exitStatus.compute_owner_query_available) 'nonzero query exit was accepted'
    Assert-StopRejected 'nonzero query exit did not fail closed'

    $env:NINFER_HARNESS_GPU_STDERR = ''
    $env:NINFER_HARNESS_GPU_EXIT = '0'
    $env:NINFER_HARNESS_GPU_STDOUT = ''
    & $ControllerPath -Action stop -StateRoot $root | Out-Null
    & $ControllerPath -Action start -StateRoot $root | Out-Null
    Assert-True ([bool](Read-Status).paused) 'idle generic start manufactured an external workload'
    [ordered]@{
        artifact_type = 'ninfer_generic_gpu_owner_regression'
        schema_version = 1
        status = 'passed'
        evidence_class = 'instrumented-unshipped-nvidia-smi-process'
        published_controller_sha256 = (Get-FileHash $publishedControllerPath -Algorithm SHA256).Hash.ToLowerInvariant()
        instrumented_controller_sha256 = (Get-FileHash $ControllerPath -Algorithm SHA256).Hash.ToLowerInvariant()
        trusted_absolute_application = $true
        actions = @('status', 'stop', 'start')
        default_mode = 'idle-host'
        active_compute_rejections = 1
        malformed_row_rejections = 1
        stderr_rejections = 1
        nonzero_exit_rejections = 1
        empty_clean_output_idle_results = 1
    } | ConvertTo-Json -Compress
}
finally {
    $env:NINFER_HARNESS_GPU_STDOUT = $originalStdout
    $env:NINFER_HARNESS_GPU_STDERR = $originalStderr
    $env:NINFER_HARNESS_GPU_EXIT = $originalExit
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $instrumentedDirectory -Recurse -Force -ErrorAction SilentlyContinue
}
