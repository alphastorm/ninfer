[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Config,
    [Parameter(Mandatory)][string]$StateRoot,
    [Parameter(Mandatory)][string]$Evidence,
    [Parameter(Mandatory)][string]$TaskSuffix,
    [Parameter(Mandatory)][ValidateSet('incumbent','candidate')][string]$Release,
    [Parameter(Mandatory)][ValidateSet('install','fault','interrupt','repair')][string]$Mode
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$settings = Get-Content -LiteralPath $Config -Raw | ConvertFrom-Json
$inputSet = $settings.$Release
$installer = [string]$inputSet.installer
if ((Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash.ToLowerInvariant() -cne $inputSet.installer_sha256) {
    throw 'installer hash differs from the frozen rehearsal input'
}
function Find-Line([string[]]$Lines, [string]$Anchor, [int]$After = 0) {
    $matches = @(for ($i = $After; $i -lt $Lines.Count; $i++) {
        if ($Lines[$i].Trim() -ceq $Anchor) { $i + 1 }
    })
    if ($matches.Count -ne 1) { throw 'installer injection anchor is missing or ambiguous' }
    return [int]$matches[0]
}
$global:RehearsalEvidence = $Evidence
$global:RehearsalMode = $Mode
$global:RehearsalPort = [int]$settings.port
$global:RehearsalBlocker = $null
$breakpoints = @()
$code = 1
try {
    if ($Release -eq 'candidate' -and $Mode -in @('fault','interrupt')) {
        $lines = @(Get-Content -LiteralPath $installer)
        $activated = Find-Line $lines "Set-TransactionFlag `$transaction 'task_touched'"
        $firstStart = Find-Line $lines '& $controllerPath -Action Start -StateRoot $StateRoot | Out-Null' $activated
        $rollback = Find-Line $lines '$failures = [Collections.Generic.List[string]]::new()'
        $breakpoints += Set-PSBreakpoint -Script $installer -Line $activated -Action {
            if ($global:RehearsalMode -eq 'interrupt') {
                # The parent verifies persisted state/journal, not this assertion alone.
                @{installer_pid=$PID;phase='candidate_activated'} | ConvertTo-Json -Compress |
                    Set-Content -LiteralPath (Join-Path $global:RehearsalEvidence 'interrupt-ready.json') -Encoding UTF8
                while ($true) { Start-Sleep -Milliseconds 100 }
            }
        }
        $breakpoints += Set-PSBreakpoint -Script $installer -Line $firstStart -Action {
            if ($global:RehearsalMode -eq 'fault') {
                $global:RehearsalBlocker = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, $global:RehearsalPort)
                $global:RehearsalBlocker.ExclusiveAddressUse = $true
                $global:RehearsalBlocker.Start()
                @{installer_pid=$PID;fault='first-start-port-occupied'} | ConvertTo-Json -Compress |
                    Set-Content -LiteralPath (Join-Path $global:RehearsalEvidence 'fault.json') -Encoding UTF8
            }
        }
        $breakpoints += Set-PSBreakpoint -Script $installer -Line $rollback -Action {
            if ($null -ne $global:RehearsalBlocker) {
                $global:RehearsalBlocker.Stop()
                $global:RehearsalBlocker = $null
            }
        }
    }
    if ($Mode -eq 'repair') {
        & $installer -StateRoot $StateRoot -RepairInterruptedInstall
    } else {
        $arguments = @{
            PackagePath=[string]$inputSet.package; PackageSha256=[string]$inputSet.package_sha256
            ModelArtifactPath=[string]$settings.model; ApiKeyFile=[string]$settings.api_key_file
            GpuUuid=[string]$settings.gpu_uuid; StateRoot=$StateRoot; ScratchTaskSuffix=$TaskSuffix
        }
        if ($Release -eq 'incumbent') { $arguments.NoStart = $true }
        & $installer @arguments
    }
    $code = 0
} catch {
    @{message=[string]$_.Exception.Message} | ConvertTo-Json -Compress |
        Set-Content -LiteralPath (Join-Path $Evidence 'failure.json') -Encoding UTF8
} finally {
    if ($null -ne $global:RehearsalBlocker) { $global:RehearsalBlocker.Stop() }
    if ($breakpoints.Count) { $breakpoints | Remove-PSBreakpoint }
}
exit $code
