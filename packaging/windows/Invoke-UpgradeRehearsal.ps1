[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Config,
    [Parameter(Mandatory)][ValidatePattern('^[A-Za-z0-9][A-Za-z0-9-]{0,22}$')][string]$Attempt,
    [Parameter(Mandatory)][ValidateSet('running','rollback','repair')][string]$Scenario
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
function Require([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function ReadJson([string]$Path) { Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json }
function Save([string]$Path, [object]$Value) {
    $temporary = $Path + '.tmp'
    [IO.File]::WriteAllText($temporary, ($Value | ConvertTo-Json -Depth 40 -Compress), [Text.UTF8Encoding]::new($false))
    Move-Item -LiteralPath $temporary -Destination $Path -Force
}
function PublishReceipt([string]$StatePath, [string]$ReceiptPath, [object]$Census) {
    $completed = ReadJson $StatePath
    Require ($completed.status -eq 'passed' -and $completed.restored -eq $true) 'cannot publish incomplete rehearsal state'
    $receipt = @{status='passed';scenario=$completed.scenario;restored=$true;census=$Census
        state_sha256=(Get-FileHash $StatePath -Algorithm SHA256).Hash.ToLowerInvariant()}
    Save $ReceiptPath $receipt
    return $receipt
}
function Quote([string]$Value) { return "'" + $Value.Replace("'", "''") + "'" }
function AssertDisjoint([string]$Mutable, [string]$Immutable) {
    $a = [IO.Path]::GetFullPath($Mutable).TrimEnd('\','/')
    $b = [IO.Path]::GetFullPath($Immutable).TrimEnd('\','/')
    Require (-not ($a.Equals($b, [StringComparison]::OrdinalIgnoreCase) -or
        $a.StartsWith($b + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -or
        $b.StartsWith($a + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase))) 'mutable output overlaps immutable input'
}
function Power {
    $text = & nvidia-smi -i ([string]$settings.gpu_uuid) --query-gpu=power.limit --format=csv,noheader,nounits
    Require ($LASTEXITCODE -eq 0) 'GPU power census failed'
    return [double]([string]$text).Trim()
}
function Canonical {
    return @($settings.canonical_roots | ForEach-Object {
        $state = ReadJson (Join-Path $_ 'state.json')
        [ordered]@{state_sha256=(Get-FileHash (Join-Path $_ 'state.json')).Hash
          task_xml=(Export-ScheduledTask -TaskName $state.task_name | Out-String)
          task_state=[string](Get-ScheduledTask -TaskName $state.task_name).State
          inner=(Test-Path (Join-Path $_ 'gpu-owner-lease.json'))
          outer=(Test-Path (Join-Path $_ 'gpu-owner-state/lease.json'))}
    })
}
function Journals([switch]$All) {
    $transactions = Join-Path $stateRoot 'receipts/install-transactions'
    if (-not (Test-Path -LiteralPath $transactions -PathType Container)) { return @() }
    return @(Get-ChildItem -LiteralPath $transactions -Directory | ForEach-Object {
        ReadJson (Join-Path $_.FullName 'journal.json')
    } | Where-Object { $All -or $_.instance_id -ceq $settings.candidate.instance_id } | Sort-Object started_utc)
}
function Observe {
    $state = ReadJson (Join-Path $stateRoot 'state.json')
    $release = $state.releases.PSObject.Properties[[string]$state.active_release].Value
    $status = (& (Join-Path $stateRoot 'Control-Release.ps1') -Action Status -StateRoot $stateRoot | Out-String) | ConvertFrom-Json
    $key = (Get-Content -LiteralPath $release.api_key_file -Raw).Trim()
    $url = 'http://' + $release.host + ':' + $release.port
    $headers = @{Authorization=('Bearer ' + $key)}
    $health = Invoke-RestMethod -Uri ($url + '/health') -Headers $headers -TimeoutSec 10
    $server = Invoke-RestMethod -Uri ($url + '/v1/ninfer/status') -Headers $headers -TimeoutSec 10
    Require ($status.process_state -eq 'running' -and $status.endpoint_state -eq 'ready' -and $health.status -eq 'ok') 'running release is not healthy'
    Require ($release.gpu_uuid -ceq $settings.gpu_uuid -and [int]$release.port -eq [int]$settings.port) 'running release GPU or port differs'
    $inner = ReadJson (Join-Path $stateRoot 'gpu-owner-lease.json')
    $outer = ReadJson (Join-Path $stateRoot 'gpu-owner-state/lease.json')
    $policy = if ($release.package_sha256 -ceq $settings.candidate.package_sha256) { $settings.candidate } else { $settings.incumbent }
    Require ((Power) -eq $policy.running_power_limit_w) 'running power policy differs'
    Require ($inner.phase -eq 'released' -and $inner.release_id -ceq $state.active_release -and
        $inner.controller_sha256 -ceq $state.gpu_owner.controller_sha256) 'inner owner lease identity differs'
    Require ($outer.paused -eq $true -and $outer.qualified_power_limit_w -eq $policy.running_power_limit_w -and
        $outer.prior_power_limit_w -eq $settings.owner_power_limit_w) 'outer owner lease policy differs'
    $observation = @{active=[string]$state.active_release; previous=$state.previous_release; prepared=$state.prepared_release
        package=[string]$release.package_sha256; owner_sha256=[string]$inner.controller_sha256
        prior_paused=[bool]$inner.prior_paused; effective_weights=$server.effective_weights}
    $script:observationNumber++
    Save (Join-Path $evidence ('observation-' + $script:observationNumber + '.json')) $observation
    return $observation
}
function Absent([object]$Journal) {
    foreach ($field in @('release_root','secret_directory','cache_root','stage_path')) {
        Require (-not (Test-Path -LiteralPath ([string]$Journal.$field))) ('candidate cleanup target remains: ' + $field)
    }
    Require (@($Journal.cleanup_actions).Count -eq 4) 'transaction lacks four cleanup actions'
}
function Install([string]$Release, [string]$Mode, [string]$Tag) {
    if ($Mode -ne 'repair') { $script:installingRelease = $Release }
    $output = Join-Path $evidence $Tag
    New-Item -ItemType Directory -Path $output -ErrorAction Stop | Out-Null
    $worker = Join-Path $PSScriptRoot 'Invoke-UpgradeInstall.ps1'
    $script = '& ' + (Quote $worker) + ' -Config ' + (Quote $Config) + ' -StateRoot ' + (Quote $stateRoot) +
        ' -Evidence ' + (Quote $output) + ' -TaskSuffix ' + (Quote $suffix) + ' -Release ' + $Release + ' -Mode ' + $Mode
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($script))
    $child = Start-Process powershell.exe -PassThru -ArgumentList @('-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass','-EncodedCommand',$encoded) -RedirectStandardOutput (Join-Path $output 'stdout.log') -RedirectStandardError (Join-Path $output 'stderr.log')
    $handle = $child.Handle # Keep a real process handle; do not reacquire a possibly recycled PID.
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $interrupted = $false
    $killRequested = $false
    try {
        while (-not $child.WaitForExit(100)) {
            $marker = Join-Path $output 'interrupt-ready.json'
            if ($Mode -eq 'interrupt' -and (Test-Path $marker)) {
                $ready = ReadJson $marker
                $pending = @(Journals)
                $state = ReadJson (Join-Path $stateRoot 'state.json')
                Require ($ready.installer_pid -eq $child.Id -and $pending.Count -eq 1 -and
                    $pending[0].phase -eq 'candidate_activated' -and $pending[0].status -eq 'in_progress' -and
                    $pending[0].flags.state_activated -eq $true -and $state.active_release -eq $pending[0].instance_id) 'interruption boundary was not durably activated'
                $killRequested = $true
                $child.Kill()
                $interrupted = $true
                break
            }
            Require ($clock.Elapsed.TotalSeconds -lt 900) 'installer deadline exceeded'
        }
        $child.WaitForExit()
        $child.Refresh()
        $code = $child.ExitCode
        if ($Mode -eq 'interrupt') { Require $interrupted 'installer did not reach the interruption boundary' }
        if ($Mode -eq 'fault') { Require (Test-Path (Join-Path $output 'fault.json')) 'first-Start fault was not injected' }
        return $code
    } finally {
        if (-not $child.HasExited) {
            if (-not $killRequested) { $killRequested = $true; $child.Kill() }
            $child.WaitForExit()
        }
        $child.Dispose()
    }
}
function InvokeScenario {
    Require ((Install 'incumbent' 'install' '01-incumbent') -eq 0) 'incumbent install failed'
    & (Join-Path $stateRoot 'Control-Release.ps1') -Action Start -StateRoot $stateRoot | Out-Null
    $before = Observe
    Require ($before.package -ceq $settings.incumbent.package_sha256) 'wrong predecessor package'
    Require ($before.prior_paused -eq $false) 'fresh predecessor unexpectedly inherited an outer pause'
    if ($Scenario -eq 'running') {
        Require ((Install 'candidate' 'install' '02-upgrade') -eq 0) 'running upgrade failed'
    } elseif ($Scenario -eq 'rollback') {
        Require ((Install 'candidate' 'fault' '02-fault') -ne 0) 'forced first Start unexpectedly succeeded'
        $journals = @(Journals)
        Require ($journals.Count -eq 1 -and $journals[0].status -eq 'rolled_back' -and $journals[0].phase -eq 'rollback_completed') 'rollback transaction incomplete'
        $failure = ReadJson (Join-Path (Split-Path $journals[0].journal_path) 'failure-receipt.json')
        Require ($failure.failure -like '*release listen port is already owned*') 'failure was not the injected first-Start refusal'
        Absent $journals[0]
        $after = Observe
        Require ($after.active -ceq $before.active -and $after.package -ceq $before.package -and
            $after.owner_sha256 -ceq $before.owner_sha256 -and $after.previous -ceq $before.previous -and
            $after.prepared -ceq $before.prepared) 'rollback did not restore the exact predecessor'
        return
    } else {
        $null = Install 'candidate' 'interrupt' '02-interrupt'
        $pending = @(Journals)
        Require ($pending.Count -eq 1 -and $pending[0].status -eq 'in_progress') 'missing interrupted transaction'
        $instance = [string]$pending[0].instance_id
        $journalPath = [string]$pending[0].journal_path
        Require ((Install 'candidate' 'repair' '03-repair') -eq 0) 'documented repair failed'
        $repaired = ReadJson $journalPath
        Require ($repaired.status -eq 'repaired' -and $repaired.phase -eq 'repair_completed') 'repair transaction incomplete'
        Absent $repaired
        Require ((Observe).active -ceq $before.active) 'repair did not restore predecessor'
        Require ((Install 'candidate' 'install' '04-reinstall') -eq 0) 'same-instance reinstall failed'
        Require ((Observe).active -ceq $instance) 'reinstall changed candidate instance'
    }
    $after = Observe
    Require ($after.active -ceq $settings.candidate.instance_id -and $after.package -ceq $settings.candidate.package_sha256 -and
        $after.previous -ceq $before.active -and $null -eq $after.prepared) 'candidate identity or lifecycle pointers differ'
    Require ($null -ne $after.effective_weights -and [Int64]$after.effective_weights.converted_device_bytes -gt 0 -and
        [Int64]$after.effective_weights.host_mapped_bytes -gt 0) 'candidate effective weight counters are not positive'
    $journals = @(Journals)
    $expected = if ($Scenario -eq 'repair') { 2 } else { 1 }
    Require ($journals.Count -eq $expected -and $journals[-1].status -eq 'completed' -and $journals[-1].phase -eq 'completed') 'candidate transaction incomplete'
    Require ($journals[-1].task_was_running -eq $true -and $journals[-1].flags.incumbent_touched -eq $true) 'upgrade did not hand off a running predecessor'
}

$settings = ReadJson $Config
Require ([string]$settings.state_root_leaf -cmatch '^[A-Za-z0-9][A-Za-z0-9._-]*$') 'invalid managed state root leaf'
$base = Join-Path ([string]$settings.output_parent) ($Attempt + '-' + $Scenario)
Require (@($settings.canonical_roots).Count -gt 0) 'canonical census roots are required'
foreach ($root in $settings.canonical_roots) { AssertDisjoint $base ([string]$root) }
foreach ($inputSet in @($settings.incumbent, $settings.candidate)) {
    AssertDisjoint $base (Split-Path -Parent $inputSet.installer)
    AssertDisjoint $base (Split-Path -Parent $inputSet.package)
    Require ((Get-FileHash $inputSet.installer -Algorithm SHA256).Hash.ToLowerInvariant() -ceq $inputSet.installer_sha256) 'installer identity mismatch'
    Require ((Get-FileHash $inputSet.package -Algorithm SHA256).Hash.ToLowerInvariant() -ceq $inputSet.package_sha256) 'package identity mismatch'
}
Require (-not (Test-Path -LiteralPath $base)) 'attempt namespace already exists; preserve it'
Require ([IO.DriveInfo]::new([IO.Path]::GetPathRoot($base)).AvailableFreeSpace -ge 60GB) 'less than 60 GiB free before scenario staging'
$beforeCanonical = ConvertTo-Json -InputObject @(Canonical) -Depth 10 -Compress
$foundPower = Power
Require ($foundPower -eq $settings.owner_power_limit_w) 'found power differs from the registered owner policy'
Require (@(Get-NetTCPConnection -State Listen -LocalPort $settings.port -ErrorAction SilentlyContinue).Count -eq 0) 'release port is not free'
New-Item -ItemType Directory -Path $base -ErrorAction Stop | Out-Null
. (Join-Path (Split-Path $settings.candidate.installer) 'Protect-StateRoot.ps1')
Set-NInferProtectedRootAcl $base
Assert-NInferProtectedStateRoot $base
$evidence = Join-Path $base 'evidence'
New-Item -ItemType Directory -Path $evidence | Out-Null
$stateRoot = Join-Path $base ([string]$settings.state_root_leaf)
$suffix = $Attempt + '-' + $Scenario
Require ($suffix.Length -le 32) 'scratch task suffix is too long'
$expectedTask = [string]$settings.task_name + '-Scratch-' + $suffix
$statePath = Join-Path $evidence 'state.json'
$script:observationNumber = 0
Save $statePath @{status='running';scenario=$Scenario}
$passed = $false
$script:installingRelease = 'incumbent'
$scenarioError = $null
$cleanupErrors = [Collections.Generic.List[string]]::new()
try {
    InvokeScenario
    $passed = $true
} catch {
    $scenarioError = $_
} finally {
    # Each independent cleanup/evidence step runs even when an earlier assertion fails.
    try {
        $pending = @(Journals -All | Where-Object { $_.status -in @('in_progress','repair_required') })
        if ($pending.Count) {
            # Early journals may not have an instance ID yet; retain the initiating installer.
            Require ((Install $script:installingRelease 'repair' 'cleanup-repair') -eq 0) 'cleanup repair failed'
        }
    } catch { $cleanupErrors.Add([string]$_.Exception.Message) }
    try {
        if (Test-Path (Join-Path $stateRoot 'state.json')) {
            $bound = ReadJson (Join-Path $stateRoot 'state.json')
            Require ($bound.task_name -ceq $expectedTask) 'refusing cleanup of a foreign task'
            $stopped = (& (Join-Path $stateRoot 'Control-Release.ps1') -Action Stop -StateRoot $stateRoot | Out-String) | ConvertFrom-Json
            Require ($stopped.process_state -eq 'stopped' -and $stopped.gpu_owner.lease_active -eq $false -and $stopped.gpu_owner.current_paused -eq $false) 'managed cleanup did not release ownership'
        }
    } catch { $cleanupErrors.Add([string]$_.Exception.Message) }
    try {
        $task = Get-ScheduledTask -TaskName $expectedTask -ErrorAction SilentlyContinue
        if ($null -ne $task) {
            if ($task.State -eq 'Running') { $cleanupErrors.Add('scratch task remains running') }
            Unregister-ScheduledTask -TaskName $expectedTask -Confirm:$false
        }
    } catch { $cleanupErrors.Add([string]$_.Exception.Message) }
    $rows = $null
    $censusError = $null
    try {
        $prefix = [IO.Path]::GetFullPath($stateRoot).TrimEnd('\') + '\'
        $rows = @(Get-CimInstance Win32_Process | Where-Object {
            $_.ExecutablePath -and ([string]$_.ExecutablePath).StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)
        } | ForEach-Object { @{pid=[int]$_.ProcessId} })
    } catch {
        $censusError = [string]$_.Exception.Message
        $cleanupErrors.Add($censusError)
    }
    try { Save (Join-Path $evidence 'census.json') @{rows=$rows;error=$censusError} }
    catch { $cleanupErrors.Add([string]$_.Exception.Message) }
    try {
        Require ($null -ne $rows -and $rows.Count -eq 0 -and (Power) -eq $foundPower) 'scratch process or power restoration incomplete'
    } catch { $cleanupErrors.Add([string]$_.Exception.Message) }
    try {
        Require (-not (Test-Path (Join-Path $stateRoot 'gpu-owner-lease.json')) -and -not (Test-Path (Join-Path $stateRoot 'gpu-owner-state/lease.json'))) 'scratch leases remain'
    } catch { $cleanupErrors.Add([string]$_.Exception.Message) }
    try {
        Require ($null -eq (Get-ScheduledTask -TaskName $expectedTask -ErrorAction SilentlyContinue)) 'scratch task remains'
    } catch { $cleanupErrors.Add([string]$_.Exception.Message) }
    try {
        Require ((ConvertTo-Json -InputObject @(Canonical) -Depth 10 -Compress) -ceq $beforeCanonical) 'canonical state changed'
    } catch { $cleanupErrors.Add([string]$_.Exception.Message) }
    $restored = $cleanupErrors.Count -eq 0
    $final = @{status=$(if ($passed -and $restored) {'passed'} else {'failed'});scenario=$Scenario
        restored=$restored;cleanup_errors=$cleanupErrors.ToArray()}
    if ($null -ne $scenarioError) { $final.failure = [string]$scenarioError.Exception.Message }
    try { Save $statePath $final }
    catch { $cleanupErrors.Add([string]$_.Exception.Message) }
}
if ($null -ne $scenarioError) { throw $scenarioError }
if ($cleanupErrors.Count) { throw ('rehearsal cleanup failed: ' + ($cleanupErrors -join '; ')) }
if ($passed) {
    $receipt = PublishReceipt $statePath (Join-Path $evidence 'receipt.json') @{rows=$rows}
    [Console]::Out.WriteLine(($receipt | ConvertTo-Json -Compress))
}
