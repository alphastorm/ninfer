"""GPU-free failure-path contracts for the native upgrade rehearsal."""
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools.qualification import driver_state as store

_spec = importlib.util.spec_from_file_location(
    'rehearsal_transport', ROOT / 'packaging/windows/run_upgrade_rehearsal.py')
upgrade = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(upgrade)


class RehearsalRegressionTests(unittest.TestCase):
    def setUp(self):
        scratch = ROOT / '.agent-tmp' / 'upgrade-rehearsal-tests'
        scratch.mkdir(parents=True, exist_ok=True)
        self.tmp = tempfile.TemporaryDirectory(dir=scratch)
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def powershell(self, body, *, functions=()):
        executable = shutil.which('pwsh')
        self.assertIsNotNone(executable, 'PowerShell is required for rehearsal contracts')
        source = str(ROOT / 'packaging/windows/Invoke-UpgradeRehearsal.ps1').replace("'", "''")
        fixture = tempfile.TemporaryDirectory(dir=self.root)
        self.addCleanup(fixture.cleanup)
        scratch_root = Path(fixture.name)
        scratch = str(scratch_root).replace("'", "''")
        names = ','.join("'" + name + "'" for name in functions)
        prefix = r"""
$ErrorActionPreference='Stop'
$source='""" + source + r"""'
$scratch='""" + scratch + r"""'
$text=Get-Content -LiteralPath $source -Raw
$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseInput($text,[ref]$tokens,[ref]$errors)
if($errors.Count){throw ($errors|Out-String)}
foreach($function in $ast.FindAll({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst]},$true)) {
 if($function.Name -in @(""" + names + r""")) {
  # Extracted functions retain their repository script directory, not this fixture's.
  Invoke-Expression $function.Extent.Text.Replace('$PSScriptRoot','(Split-Path $source)')
 }
}
"""
        path = scratch_root / 'contract.ps1'
        path.write_text(prefix + body, encoding='utf-8')
        result = subprocess.run([executable, '-NoProfile', '-NonInteractive', '-File', str(path)],
                                text=True, capture_output=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def scenario(self, body):
        self.powershell(r"""
$stateRoot=$scratch
Set-Content (Join-Path $stateRoot 'Control-Release.ps1') 'param($Action,$StateRoot)'
$settings=@{incumbent=@{package_sha256='old';instance_id='old-id'}
 candidate=@{package_sha256='new';instance_id='new-id'}}
$script:changedPointer=$null
$script:candidateIdentity='new-id'
function Observe {return $script:active}
function Journals {return $script:journals}
function ReadJson($Path) {
 if($Path -like '*failure-receipt.json'){return @{failure='release listen port is already owned'}}
 return @{status='repaired';phase='repair_completed'}
}
function Absent($Journal) {}
function Install($Release,$Mode,$Tag) {
 if($Release -eq 'incumbent' -or $Mode -eq 'repair') {
  $script:active=@{active='old-id';package='old';prior_paused=$false;owner_sha256='owner'
   previous='prior-previous';prepared='prior-prepared'}
  return 0
 }
 if($Mode -eq 'fault') {
  $script:active=$script:active.Clone()
  if($script:changedPointer){$script:active[$script:changedPointer]='unexpected-pointer'}
  $script:journals=@(@{status='rolled_back';phase='rollback_completed';journal_path=(Join-Path $stateRoot 'journal.json')})
  return 1
 }
 if($Mode -eq 'interrupt') {
  $script:journals=@(@{status='in_progress';instance_id='new-id';journal_path='pending'})
  return 1
 }
 $script:active=@{active=$script:candidateIdentity;package='new';previous='old-id';prepared=$null
  effective_weights=@{converted_device_bytes=1;host_mapped_bytes=1}}
 $script:journals+=@{instance_id='new-id';status='completed';phase='completed';task_was_running=$true;flags=@{incumbent_touched=$true}}
 return 0
}
""" + body, functions=('InvokeScenario', 'Require'))

    def cleanup(self, setup, assertions):
        self.powershell(r"""
$stateRoot=Join-Path $scratch 'managed'
$evidence=Join-Path $scratch 'evidence'
New-Item -ItemType Directory -Path $stateRoot,$evidence | Out-Null
$statePath=Join-Path $evidence 'state.json'
$Scenario='running'
$expectedTask='Rehearsal-Scratch-attempt-running'
$settings=@{incumbent=@{package_sha256='old'};candidate=@{instance_id='new-id'}}
$foundPower=100
$beforeCanonical='[]'
$script:repairs=@()
$script:task=[pscustomobject]@{State='Ready'}
$script:unregistered=$false
$script:published=$false
$script:scenarioFails=$true
$script:censusFails=$false
function InvokeScenario {if($script:scenarioFails){throw 'primary-scenario-failure'}}
function Install($Release,$Mode,$Tag) {
 $script:repairs+=($Release+'/'+$Mode)
 foreach($journal in @(Journals -All)) {
  $journal.status='repaired'
  Save $journal.journal_path $journal
 }
 return 0
}
function Get-ScheduledTask {param($TaskName,$ErrorAction) return $script:task}
function Unregister-ScheduledTask {param($TaskName,[switch]$Confirm)
 if($TaskName -cne $expectedTask){throw 'unregistered a foreign task'}
 $script:unregistered=$true;$script:task=$null
}
function Get-CimInstance {param($ClassName)
 if($script:censusFails){throw 'census-provider-failure'}
 return @()
}
function Power {return 100}
function Canonical {return @()}
function PublishReceipt {param($StatePath,$ReceiptPath,$Census)
 $script:published=$true;return @{status='passed'}
}
""" + setup + r"""
$start=@($ast.EndBlock.Statements | Where-Object {
 $_ -is [Management.Automation.Language.AssignmentStatementAst] -and $_.Left.Extent.Text -ceq '$passed'
})[0].Extent.StartOffset
$caught=$null
try {Invoke-Expression $text.Substring($start)} catch {$caught=$_.Exception.Message}
""" + assertions, functions=('Require', 'ReadJson', 'Save', 'Journals', 'ScratchTask'))

    def test_rollback_rejects_changed_lifecycle_pointers(self):
        for field in ('previous', 'prepared'):
            with self.subTest(field=field):
                self.scenario("$script:changedPointer='" + field + "'\n" + r"""
$Scenario='rollback';$script:journals=@()
$failure=$null
try {InvokeScenario} catch {$failure=$_.Exception.Message}
if($failure -cne 'rollback did not restore the exact predecessor'){throw ('accepted changed predecessor '+$script:changedPointer)}
""")

    def test_running_upgrade_rejects_wrong_candidate_instance(self):
        self.scenario(r"""
$Scenario='running';$script:journals=@();$script:candidateIdentity='different-id'
$failure=$null
try {InvokeScenario} catch {$failure=$_.Exception.Message}
if($failure -cne 'candidate identity or lifecycle pointers differ'){throw 'accepted the wrong active candidate instance'}
""")

    def test_scenarios_accept_exact_restoration_and_candidate_identity(self):
        self.scenario(r"""
foreach($Scenario in @('running','rollback','repair')) {
 $script:journals=@()
 InvokeScenario
}
""")

    def test_transport_timeout_retains_partial_logs_and_failed_state(self):
        config = self.root / 'config.json'
        store.atomic_json(config, {'evidence_parent': str(self.root / 'evidence'),
                                  'immutable_roots': [str(self.root / 'inputs')],
                                  'command': ['transport'], 'lock_command': ['lock'],
                                  'timeout_seconds': 1})
        original_open = Path.open

        def windows_log_open(path, mode='r', *args, **kwargs):
            # Exercise Windows text-mode translation even on a POSIX test runner.
            if path.suffix == '.log' and 'b' not in mode and kwargs.get('newline') is None:
                kwargs['newline'] = '\r\n'
            return original_open(path, mode, *args, **kwargs)

        for index, (stdout, stderr, expected_out, expected_err) in enumerate((
                (b'partial\xff progress\n', b'partial error\n', b'partial\xff progress\n', b'partial error\n'),
                ('text progress\n', 'text error\n', b'text progress\n', b'text error\n'),
                (None, None, b'', b''))):
            with self.subTest(index=index):
                failure = subprocess.TimeoutExpired(['transport'], 1, output=stdout, stderr=stderr)
                attempt = 'timeout-' + str(index)
                with patch.object(Path, 'open', windows_log_open), \
                        patch.object(upgrade.subprocess, 'run', side_effect=failure):
                    with self.assertRaises(subprocess.TimeoutExpired):
                        upgrade.execute(config, attempt)
                directory = self.root / 'evidence' / attempt
                self.assertEqual(json.loads((directory / 'running.state.json').read_text())['status'], 'failed')
                self.assertFalse((directory / 'running.json').exists())
                self.assertEqual((directory / 'running.stdout.log').read_bytes(), expected_out)
                self.assertEqual((directory / 'running.stderr.log').read_bytes(), expected_err)

    def test_cleanup_repairs_incumbent_but_preserves_unowned_task_without_state(self):
        for instance in ("'old-id'", '$null'):
            with self.subTest(instance=instance):
                self.cleanup('$journalInstance=' + instance + '\n' + r"""
$journalPath=Join-Path $stateRoot 'receipts/install-transactions/incumbent/journal.json'
New-Item -ItemType Directory -Path (Split-Path $journalPath) | Out-Null
Save $journalPath @{instance_id=$journalInstance;status='in_progress';started_utc='2026-01-01';journal_path=$journalPath}
""", r"""
if($caught -cne 'primary-scenario-failure'){throw 'lost the interrupted-incumbent failure'}
if(($script:repairs -join ',') -cne 'incumbent/repair'){throw 'did not repair the incumbent journal'}
if($script:unregistered){throw 'unregistered a task without lifecycle ownership state'}
$final=ReadJson $statePath
if($final.status -cne 'failed' -or $final.restored -ne $false){throw 'interrupted-incumbent cleanup state is incorrect'}
if($final.cleanup_errors -notcontains 'refusing to unregister scratch task without matching lifecycle state'){throw 'missing ownership refusal evidence'}
if(-not (Test-Path (Join-Path $evidence 'census.json'))){throw 'cleanup census missing'}
""")

    def test_cleanup_refuses_task_owned_by_another_scratch_root(self):
        for binding in ('missing', 'foreign'):
            with self.subTest(binding=binding):
                setup = r"""
$script:scenarioFails=$false
$otherRoot=Join-Path $scratch 'other-output/managed'
New-Item -ItemType Directory -Path $otherRoot -Force | Out-Null
Save (Join-Path $otherRoot 'state.json') @{task_name=$expectedTask}
$script:task | Add-Member -NotePropertyName StateRoot -NotePropertyValue $otherRoot
"""
                if binding == 'foreign':
                    setup += "Save (Join-Path $stateRoot 'state.json') @{task_name='foreign-task'}\n"
                self.cleanup(setup, r"""
if($script:unregistered -or $null -eq $script:task){throw 'removed another scratch root task'}
if((ReadJson (Join-Path $otherRoot 'state.json')).task_name -cne $expectedTask){throw 'changed another scratch root state'}
if(-not $caught){throw 'accepted cleanup without task ownership'}
$final=ReadJson $statePath
if($final.status -cne 'failed' -or $final.restored -ne $false){throw 'reported restoration without task ownership'}
if($final.cleanup_errors -notcontains 'refusing to unregister scratch task without matching lifecycle state'){throw 'missing ownership refusal evidence'}
if($script:published){throw 'published a receipt without task ownership'}
""")

    def test_cleanup_records_nonterminating_task_lookup_failures(self):
        for failing_probe in (1, 2):
            with self.subTest(failing_probe=failing_probe):
                self.cleanup('$script:failingProbe=' + str(failing_probe) + '\n' + r"""
$script:scenarioFails=$false
$script:taskProbes=0
Save (Join-Path $stateRoot 'state.json') @{task_name=$expectedTask}
Set-Content (Join-Path $stateRoot 'Control-Release.ps1') @'
param($Action,$StateRoot)
@{process_state='stopped';gpu_owner=@{lease_active=$false;current_paused=$false}} | ConvertTo-Json -Compress
'@
function Get-ScheduledTask {
 [CmdletBinding()]
 param([string]$TaskName)
 $script:taskProbes++
 if($script:taskProbes -ge $script:failingProbe) {
  Write-Error 'scratch-task-provider-failure' -ErrorId ScratchTaskProviderFailure -Category ResourceUnavailable
  return
 }
 return $script:task
}
function Unregister-ScheduledTask {param($TaskName,[switch]$Confirm)
 $script:unregistered=$true
 # The task remains registered, so a failed final probe cannot prove absence.
}
""", r"""
$final=ReadJson $statePath
if($final.cleanup_errors -notcontains 'scratch-task-provider-failure'){throw 'scheduler lookup failure was not retained'}
if($final.status -cne 'failed' -or $final.restored -ne $false){throw 'reported restoration after a failed task lookup'}
if(-not $caught -or $null -eq $script:task){throw 'accepted an unobserved registered task'}
if($script:taskProbes -ne 2){throw 'skipped an independent scratch task probe'}
if($script:published){throw 'published a receipt after a failed task lookup'}
""")

    def test_cleanup_accepts_only_scheduler_task_not_found_as_absent(self):
        for error_id in ('CmdletizationQuery_NotFound_TaskName', 'TaskProviderUnavailable'):
            with self.subTest(error_id=error_id):
                self.cleanup("$script:taskErrorId='" + error_id + "'\n" + r"""
$script:scenarioFails=$false
$script:task=$null
function Get-ScheduledTask {
 [CmdletBinding()]
 param([string]$TaskName)
 Write-Error 'task lookup returned no object' -ErrorId $script:taskErrorId -Category ObjectNotFound
}
""", r"""
$final=ReadJson $statePath
if($script:taskErrorId -ceq 'CmdletizationQuery_NotFound_TaskName') {
 if($caught -or $final.status -cne 'passed' -or $final.restored -ne $true){throw 'task-not-found was not treated as absent'}
 if(-not $script:published){throw 'did not publish a restored no-task scenario'}
} else {
 if(-not $caught -or $final.restored -ne $false){throw 'unrelated ObjectNotFound error was treated as task absence'}
 if($final.cleanup_errors -notcontains 'task lookup returned no object'){throw 'lookup failure evidence missing'}
 if($script:published){throw 'published a receipt after an unrelated lookup error'}
}
if($script:unregistered){throw 'unregistered an absent task'}
""")

    def test_cleanup_repairs_early_candidate_journal_with_no_instance_yet(self):
        self.cleanup(r"""
function InvokeScenario {$script:installingRelease='candidate';throw 'primary-scenario-failure'}
$journalPath=Join-Path $stateRoot 'receipts/install-transactions/candidate/journal.json'
New-Item -ItemType Directory -Path (Split-Path $journalPath) | Out-Null
Save $journalPath @{instance_id=$null;status='in_progress';started_utc='2026-01-01';journal_path=$journalPath}
""", r"""
if($caught -cne 'primary-scenario-failure'){throw 'lost the early candidate failure'}
if(($script:repairs -join ',') -cne 'candidate/repair'){throw 'did not use the interrupted candidate installer'}
if($script:unregistered -or (ReadJson $statePath).restored -ne $false){throw 'early candidate cleanup claimed task ownership without state'}
""")

    def test_missing_transaction_directory_does_not_abort_predecessor_stop(self):
        self.cleanup(r"""
Save (Join-Path $stateRoot 'state.json') @{task_name=$expectedTask}
Set-Content (Join-Path $stateRoot 'Control-Release.ps1') @'
param($Action,$StateRoot)
Set-Content (Join-Path $StateRoot 'stopped.txt') $Action
@{process_state='stopped';gpu_owner=@{lease_active=$false;current_paused=$false}} | ConvertTo-Json -Compress
'@
""", r"""
if($caught -cne 'primary-scenario-failure'){throw 'missing journal directory masked the primary failure'}
if((Get-Content (Join-Path $stateRoot 'stopped.txt') -Raw).Trim() -cne 'Stop'){throw 'predecessor Stop was skipped'}
if(-not $script:unregistered){throw 'predecessor task was not unregistered'}
if((ReadJson $statePath).restored -ne $true){throw 'missing transaction directory was not treated as no journals'}
""")

    def test_cleanup_assertion_preserves_primary_and_all_final_evidence(self):
        self.cleanup(r"""
New-Item -ItemType Directory -Path (Join-Path $stateRoot 'receipts/install-transactions') | Out-Null
Save (Join-Path $stateRoot 'state.json') @{task_name=$expectedTask}
Set-Content (Join-Path $stateRoot 'Control-Release.ps1') @'
param($Action,$StateRoot)
@{process_state='running';gpu_owner=@{lease_active=$true;current_paused=$true}} | ConvertTo-Json -Compress
'@
""", r"""
if($caught -cne 'primary-scenario-failure'){throw 'cleanup assertion masked the primary scenario exception'}
if(-not $script:unregistered){throw 'cleanup assertion skipped task unregistration'}
if(-not (Test-Path (Join-Path $evidence 'census.json'))){throw 'cleanup assertion skipped census persistence'}
$final=ReadJson $statePath
if($final.status -cne 'failed' -or $final.restored -ne $false){throw 'cleanup assertion skipped failed final state'}
if($final.cleanup_errors -notcontains 'managed cleanup did not release ownership'){throw 'cleanup failure evidence missing'}
if($script:published){throw 'published a receipt after failed cleanup'}
""")

    def test_cleanup_running_task_is_unregistered_and_census_failure_is_recorded(self):
        self.cleanup(r"""
$script:scenarioFails=$false
Save (Join-Path $stateRoot 'state.json') @{task_name=$expectedTask}
Set-Content (Join-Path $stateRoot 'Control-Release.ps1') @'
param($Action,$StateRoot)
@{process_state='stopped';gpu_owner=@{lease_active=$false;current_paused=$false}} | ConvertTo-Json -Compress
'@
$script:task.State='Running'
$script:censusFails=$true
""", r"""
if(-not $caught){throw 'cleanup failure was accepted'}
if(-not $script:unregistered){throw 'running-task assertion skipped unregistration'}
$census=ReadJson (Join-Path $evidence 'census.json')
if($null -ne $census.rows -or $census.error -cne 'census-provider-failure'){throw 'failed census was not retained truthfully'}
$final=ReadJson $statePath
if($final.status -cne 'failed' -or $final.restored -ne $false){throw 'cleanup-only failure was not persisted'}
if($final.cleanup_errors -notcontains 'scratch task remains running'){throw 'running-task assertion was lost'}
if($final.cleanup_errors -notcontains 'census-provider-failure'){throw 'census failure was lost'}
if($script:published){throw 'published a receipt after failed cleanup'}
""")

    def test_output_rejects_canonical_descendants_before_namespace_creation(self):
        self.powershell(r"""
$inputs=Join-Path $scratch 'inputs'
New-Item -ItemType Directory -Path $inputs | Out-Null
$installer=Join-Path $inputs 'installer.ps1';$package=Join-Path $inputs 'package.zip'
Set-Content $installer 'installer';Set-Content $package 'package'
$inputSet=@{installer=$installer;package=$package
 installer_sha256=(Get-FileHash $installer -Algorithm SHA256).Hash.ToLowerInvariant()
 package_sha256=(Get-FileHash $package -Algorithm SHA256).Hash.ToLowerInvariant()}
$canonical=Join-Path $scratch 'canonical'
$settings=@{incumbent=$inputSet;candidate=$inputSet;canonical_roots=@((Join-Path $scratch 'other-canonical'),$canonical)
 output_parent=(Join-Path $canonical 'scratch')}
$Attempt='attempt';$Scenario='running'
$start=@($ast.EndBlock.Statements | Where-Object {
 $_ -is [Management.Automation.Language.AssignmentStatementAst] -and $_.Left.Extent.Text -ceq '$base'
})[0].Extent.StartOffset
$end=@($ast.EndBlock.Statements | Where-Object {$_.Extent.Text -like '*less than 60 GiB free before scenario staging*'})[0].Extent.StartOffset
$failure=$null
try {Invoke-Expression $text.Substring($start,$end-$start)} catch {$failure=$_.Exception.Message}
if($failure -cne 'mutable output overlaps immutable input'){throw 'accepted output nested inside a canonical root'}
if(Test-Path $base){throw 'created the rejected output namespace'}
""", functions=('Require', 'AssertDisjoint'))

    def test_interrupt_kills_once_even_when_process_exit_is_delayed(self):
        self.powershell(r"""
class RehearsalChild {
 [int]$Id=42
 [int]$Handle=1
 [int]$ExitCode=137
 [bool]$HasExited=$false
 [int]$Kills=0
 [int]$Polls=0
 [bool]$Disposed=$false
 [bool] WaitForExit([int]$Milliseconds) {$this.Polls++;return $this.Polls -ge 3}
 [void] WaitForExit() {$this.HasExited=$true}
 [void] Kill() {$this.Kills++;if($this.Kills -gt 1){throw 'Kill invoked twice'}}
 [void] Refresh() {}
 [void] Dispose() {$this.Disposed=$true}
}
$script:child=[RehearsalChild]::new()
$evidence=Join-Path $scratch 'evidence';$stateRoot=Join-Path $scratch 'managed'
New-Item -ItemType Directory -Path $evidence,$stateRoot | Out-Null
Save (Join-Path $stateRoot 'state.json') @{active_release='new-id'}
$Config='config';$suffix='attempt-repair'
function Start-Process {param($FilePath,[switch]$PassThru,$ArgumentList,$RedirectStandardOutput,$RedirectStandardError)
 Save (Join-Path (Split-Path $RedirectStandardOutput) 'interrupt-ready.json') @{installer_pid=42}
 return $script:child
}
function Journals {return @(@{instance_id='new-id';phase='candidate_activated';status='in_progress';flags=@{state_activated=$true}})}
$code=Install 'candidate' 'interrupt' '02-interrupt'
if($code -ne 137 -or $script:child.Kills -ne 1 -or -not $script:child.Disposed){throw 'interruption did not terminate exactly once'}
if($script:installingRelease -cne 'candidate'){throw 'did not retain the installer identity for cleanup'}
""", functions=('Install', 'Require', 'Quote', 'ReadJson', 'Save'))

    def test_interrupt_and_fault_markers_are_published_by_atomic_rename(self):
        worker = str(ROOT / 'packaging/windows/Invoke-UpgradeInstall.ps1').replace("'", "''")
        self.powershell(r"""
$worker='""" + worker + r"""'
$installer=Join-Path $scratch 'installer.ps1'
Set-Content $installer @'
param($StateRoot,$PackagePath,$PackageSha256,$ModelArtifactPath,$ApiKeyFile,$GpuUuid,$ScratchTaskSuffix,[switch]$NoStart)
if($false) {
 Set-TransactionFlag $transaction 'task_touched'
 & $controllerPath -Action Start -StateRoot $StateRoot | Out-Null
 $failures = [Collections.Generic.List[string]]::new()
}
'@
$Config=Join-Path $scratch 'config.json';$Evidence=$scratch;$StateRoot=$scratch;$TaskSuffix='attempt-repair'
$Release='candidate';$Mode='fault'
@{candidate=@{installer=$installer;installer_sha256=(Get-FileHash $installer -Algorithm SHA256).Hash.ToLowerInvariant()};port=0} |
 ConvertTo-Json -Compress | Set-Content $Config
$script:actions=@();$script:moves=@()
function Set-PSBreakpoint {param($Script,$Line,$Action) $script:actions+=,$Action;return @{id=$Line}}
function Remove-PSBreakpoint {param([Parameter(ValueFromPipeline)]$Breakpoint)}
function Move-Item {param($LiteralPath,$Destination,[switch]$Force)
 if($Destination -like '*interrupt-ready.json' -or $Destination -like '*fault.json') {
  if(Test-Path $Destination){throw 'marker appeared before its atomic rename'}
  if(-not $LiteralPath.EndsWith('.tmp')){throw 'marker was not staged in a temporary file'}
  $null=Get-Content -LiteralPath $LiteralPath -Raw | ConvertFrom-Json
  $script:moves+=,$Destination
 }
 Microsoft.PowerShell.Management\Move-Item -LiteralPath $LiteralPath -Destination $Destination -Force:$Force
}
function Start-Sleep {param($Milliseconds) throw 'marker-published'}
$tokens=$null;$errors=$null
$workerAst=[Management.Automation.Language.Parser]::ParseFile($worker,[ref]$tokens,[ref]$errors)
if($errors.Count){throw ($errors|Out-String)}
foreach($statement in $workerAst.EndBlock.Statements) {
 if($statement -is [Management.Automation.Language.ExitStatementAst]){break}
 Invoke-Expression $statement.Extent.Text
}
$global:RehearsalMode='interrupt'
try {& $script:actions[0]} catch {if($_.Exception.Message -cne 'marker-published'){throw}}
if($script:moves.Count -ne 1 -or -not (Test-Path (Join-Path $Evidence 'interrupt-ready.json'))){throw 'interrupt marker was not published atomically'}
$global:RehearsalMode='fault'
try {& $script:actions[1]} finally {
 if($null -ne $global:RehearsalBlocker){$global:RehearsalBlocker.Stop()}
}
if($script:moves.Count -ne 2 -or -not (Test-Path (Join-Path $Evidence 'fault.json'))){throw 'fault marker was not published atomically'}
""")


if __name__ == '__main__':
    unittest.main()
