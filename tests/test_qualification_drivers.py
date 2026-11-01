"""GPU-free regressions distilled from the private release wrappers."""
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import shutil
import subprocess
import sys
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools.qualification import driver_state as driver
from tools.qualification import run_native_chain as chain
import importlib.util
_spec = importlib.util.spec_from_file_location('upgrade_driver', ROOT / 'packaging/windows/run_upgrade_rehearsal.py')
upgrade = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(upgrade)


class DriverInvariantTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def test_attempt_namespace_refuses_reuse(self):
        first = driver.claim_attempt(self.root, 'attempt-a')
        (first / 'corpus').mkdir()
        with self.assertRaises((ValueError, FileExistsError)):
            driver.claim_attempt(self.root, 'attempt-a')
        self.assertNotEqual(first, driver.claim_attempt(self.root, 'attempt-b'))

    def test_attempt_name_cannot_escape_namespace(self):
        for name in ('../escape', '.', 'nested/name', ''):
            with self.subTest(name=name), self.assertRaises(ValueError):
                driver.claim_attempt(self.root, name)

    def test_full_document_accepts_pretty_json(self):
        value = {'power_limit_w': 100, 'rows': [{'pid': 17}]}
        self.assertEqual(driver.parse_object(json.dumps(value, indent=2)), value)

    def test_compact_consumer_rejects_pretty_and_nested_json(self):
        for text in ('{\n "rows": []\n}', '{\n"nested":\n{"status":"passed"}\n}'):
            with self.subTest(text=text), self.assertRaises(ValueError):
                driver.parse_object(text, compact=True)

    def test_compact_consumer_accepts_single_object(self):
        self.assertEqual(driver.parse_object('{"rows":[]}\r\n', compact=True), {'rows': []})

    def test_json_boundary_rejects_ambiguous_or_nonobject_output(self):
        for text in ('[]', 'null', '{}\n{}', 'noise\n{"status":"passed"}'):
            with self.subTest(text=text), self.assertRaises(ValueError):
                driver.parse_object(text)

    def test_census_requires_array_for_zero_one_and_many_rows(self):
        for rows in ([], [{'pid': 1}], [{'pid': 1}, {'pid': 2}]):
            self.assertEqual(driver.require_census({'rows': rows}), rows)
        for rows in (None, {'pid': 1}, 0, 'none', [1]):
            with self.subTest(rows=rows), self.assertRaises(ValueError):
                driver.require_census({'rows': rows})

    def test_logs_cannot_create_immutable_stage_root(self):
        for logs in ('Q:/inputs/logs', 'q:\\INPUTS\\transport.log', 'Q:/inputs', 'Q:/inputs/sub/../log'):
            with self.subTest(logs=logs), self.assertRaises(ValueError):
                driver.require_disjoint_logs(logs, ['Q:/inputs'])
        driver.require_disjoint_logs('Q:/evidence', ['Q:/inputs'])

    def test_logs_and_inputs_cannot_overlap_in_reverse(self):
        with self.assertRaises(ValueError):
            driver.require_disjoint_logs('Q:/work', ['Q:/work/inputs'])

    def test_state_persisted_before_completed_receipt(self):
        state_path, receipt_path = self.root / 'state.json', self.root / 'receipt.json'
        driver.atomic_json(state_path, {'status': 'running'})
        writes = []
        original = driver.atomic_json
        def observe(path, value):
            if path == receipt_path:
                self.assertEqual(json.loads(state_path.read_text())['status'], 'passed')
            writes.append(path.name)
            original(path, value)
        with patch.object(driver, 'atomic_json', side_effect=observe):
            driver.publish_completed(state_path, receipt_path, {'status': 'running'}, {'status': 'passed'})
        self.assertEqual(writes, ['state.json', 'receipt.json'])
        self.assertEqual(driver.completed_receipt(state_path, receipt_path)['status'], 'passed')

    def test_continuation_refuses_incomplete_or_mismatched_receipt(self):
        state_path, receipt_path = self.root / 'state.json', self.root / 'receipt.json'
        driver.atomic_json(state_path, {'status': 'running'})
        driver.atomic_json(receipt_path, {'status': 'passed'})
        with self.assertRaises(ValueError):
            driver.completed_receipt(state_path, receipt_path)
        driver.publish_completed(state_path, receipt_path, {'status': 'running'}, {'status': 'passed'})
        driver.atomic_json(state_path, {'status': 'passed', 'changed': True})
        with self.assertRaises(ValueError):
            driver.completed_receipt(state_path, receipt_path)

    def test_crash_after_state_before_receipt_cannot_continue(self):
        state_path, receipt_path = self.root / 'state.json', self.root / 'receipt.json'
        original = driver.atomic_json
        def crash(path, value):
            if path == receipt_path:
                raise OSError('simulated publication interruption')
            original(path, value)
        with patch.object(driver, 'atomic_json', side_effect=crash), self.assertRaises(OSError):
            driver.publish_completed(state_path, receipt_path, {}, {'status': 'passed'})
        self.assertFalse(receipt_path.exists())
        with self.assertRaises((ValueError, FileNotFoundError)):
            driver.completed_receipt(state_path, receipt_path)

    def test_native_json_boundary_accepts_pretty_but_not_truncated_nested_object(self):
        subject = object.__new__(chain.ChainOrchestrator)
        subject.config = SimpleNamespace(target='target')
        for text in ('{"rows":[]}', '{\n"rows": []\n}', 'progress\n{"rows":[]}',
                     'progress\n{\n"rows":[]\n}'):
            with patch.object(chain.native, 'remote_ps', return_value=text):
                self.assertEqual(subject.remote_json('builder', 'script'), {'rows': []})
        for text in ('{\n"nested":\n{"status":"passed"}', '{\n"rows":[]\n}\nnoise',
                     'progress\n{\n"nested":\n{"status":"passed"}', '{}\n{}'):
            with patch.object(chain.native, 'remote_ps', return_value=text), self.assertRaises(ValueError):
                subject.remote_json('builder', 'script')

    def test_native_chain_failure_then_fresh_recovery_and_completion(self):
        import dataclasses
        values = {field.name: '' for field in dataclasses.fields(chain.native.Config)}
        values.update(source_root=str(self.root / 'source'), long_fixture=str(self.root / 'fixture'),
                      transfer_tool=None, scratch_state_root='Q:/work/{attempt}-target/managed')
        config_path = self.root / 'config.json'
        driver.atomic_json(config_path, {'evidence_parent': str(self.root / 'outputs'),
                                        'builder_parent': 'Q:/work', 'target_parent': 'Q:/work',
                                        'native': values, 'expected_package_sha256': 'package'})
        failure = ['protocol']
        calls = []

        class FakeOrchestrator:
            def __init__(self, config):
                self.config = config
                self.state_path = config.state_dir / 'state.json'
                self.lane = SimpleNamespace(name='lane')
                self.state = (json.loads(self.state_path.read_text()) if self.state_path.exists()
                              else {'status': 'running', 'phases': {}})

            def _save(self):
                driver.atomic_json(self.state_path, self.state)

            def remote_json(self, *args):
                return {'status': 'passed'}

            def __getattr__(self, name):
                return lambda: {'status': 'passed'}

            def phase(self, name, action):
                calls.append(name)
                if failure and name == failure[0]:
                    raise RuntimeError('injected phase failure')
                receipt = action()
                if name == 'package':
                    receipt['package'] = {'sha256': 'package'}
                path = self.config.state_dir / 'receipts' / (name + '.json')
                driver.atomic_json(path, receipt)
                self.state['phases'][name] = {'status': 'passed', 'receipt': receipt,
                                             'receipt_sha256': driver.file_sha256(path)}
                if name == 'receipt':
                    driver.atomic_json(self.config.state_dir / 'qualification-summary.json', receipt)
                self._save()

            def cleanup(self):
                self.state['restoration'] = {'status': 'passed'}
                self._save()

        def compose(state, summary, lane):
            path = self.root / 'outputs' / 'recovery' / 'native' / 'state.json'
            self.assertEqual(json.loads(path.read_text())['status'], 'passed')
            self.assertEqual(state['status'], 'passed')
            return {'status': 'passed'}

        with patch.object(chain, 'ChainOrchestrator', FakeOrchestrator), \
                patch.object(chain.native, 'stage_lane_qualification', side_effect=compose):
            chain.run_part(config_path, 'initial', 'cpu')
            with self.assertRaises(RuntimeError):
                chain.run_part(config_path, 'initial', 'protocol')
            with self.assertRaises((ValueError, FileNotFoundError)):
                chain.run_part(config_path, 'initial', 'correctness')
            failure.clear()
            for part in chain.GROUPS:
                chain.run_part(config_path, 'recovery', part)
            self.assertEqual(json.loads((self.root / 'outputs/recovery/native/lane.json').read_text()),
                             {'status': 'passed'})
            with self.assertRaises(FileExistsError):
                chain.run_part(config_path, 'recovery', 'cpu')
            phase = self.root / 'outputs/recovery/native/receipts/protocol.json'
            phase.write_text('{}')
            state = json.loads((self.root / 'outputs/recovery/native/state.json').read_text())
            with self.assertRaises(ValueError):
                driver.require_phases(state, phase.parents[1], ['protocol'])

    def run_powershell(self, script):
        executable = shutil.which('pwsh')
        self.assertIsNotNone(executable, 'CI must provide PowerShell for driver contracts')
        path = self.root / 'contract.ps1'
        path.write_text(script, encoding='utf-8')
        result = subprocess.run([executable, '-NoProfile', '-NonInteractive', '-File', str(path)],
                                text=True, capture_output=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_upgrade_transport_failure_then_fresh_completed_chain(self):
        config = self.root / 'upgrade.json'
        driver.atomic_json(config, {'evidence_parent': str(self.root / 'evidence'),
                                   'immutable_roots': [str(self.root / 'inputs')],
                                   'command': ['transport'], 'lock_command': ['lock'],
                                   'timeout_seconds': 60})
        def response(command, **kwargs):
            value = {'status': 'passed', 'scenario': command[-1], 'restored': True,
                     'census': {'rows': []}, 'state_sha256': 'a' * 64}
            return subprocess.CompletedProcess(command, 0, json.dumps(value), '')
        with patch.object(upgrade.subprocess, 'run', return_value=subprocess.CompletedProcess([], 1, '', 'failed')):
            with self.assertRaises(subprocess.CalledProcessError):
                upgrade.execute(config, 'failed')
        self.assertFalse((self.root / 'evidence/failed/running.json').exists())
        with patch.object(upgrade.subprocess, 'run', side_effect=response) as transport:
            upgrade.execute(config, 'recovery')
            self.assertEqual([call.args[0][-1] for call in transport.call_args_list], list(upgrade.SCENARIOS))
        for scenario in upgrade.SCENARIOS:
            output = self.root / 'evidence/recovery'
            receipt = driver.completed_receipt(output / (scenario + '.state.json'), output / (scenario + '.json'))
            self.assertEqual(receipt['remote_state_sha256'], 'a' * 64)
        with self.assertRaises(FileExistsError):
            upgrade.execute(config, 'recovery')

    def test_powershell_parsing_anchors_and_census_cardinality(self):
        source = str(ROOT / 'packaging/windows').replace("'", "''")
        self.run_powershell(r"""
$ErrorActionPreference='Stop'
$source='""" + source + r"""'
foreach($name in @('Invoke-UpgradeInstall.ps1','Invoke-UpgradeRehearsal.ps1')) {
 $tokens=$null;$errors=$null
 $ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $source $name),[ref]$tokens,[ref]$errors)
 if($errors.Count){throw ($errors|Out-String)}
 foreach($function in $ast.FindAll({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst]},$true)) {
  if($function.Name -in @('Find-Line','AssertDisjoint','Require','Save','ReadJson','PublishReceipt')) {Invoke-Expression $function.Extent.Text}
 }
}
$lines=@(Get-Content (Join-Path $source 'Install-Release.ps1'))
$activation=Find-Line $lines "Set-TransactionFlag `$transaction 'task_touched'"
$start=Find-Line $lines '& $controllerPath -Action Start -StateRoot $StateRoot | Out-Null' $activation
if($start -le $activation){throw 'first Start precedes activation'}
$null=Find-Line $lines '$failures = [Collections.Generic.List[string]]::new()'
foreach($count in @(0,1,2)) {
 $rows=@(for($i=0;$i -lt $count;$i++){@{pid=$i}})
 $decoded=(@{rows=$rows}|ConvertTo-Json -Compress|ConvertFrom-Json)
 if($decoded.rows -isnot [Array] -or $decoded.rows.Count -ne $count){throw 'census cardinality changed'}
}
$root=Join-Path ([IO.Path]::GetTempPath()) 'driver-inputs'
try {AssertDisjoint (Join-Path $root 'logs') $root;throw 'accepted-overlap'} catch {
 if($_.Exception.Message -eq 'accepted-overlap'){throw}
}
$state=Join-Path $PSScriptRoot 'state.json';$receipt=Join-Path $PSScriptRoot 'receipt.json'
Save $state @{status='running';restored=$false}
try {$null=PublishReceipt $state $receipt @{rows=@()};throw 'accepted-incomplete'} catch {
 if($_.Exception.Message -eq 'accepted-incomplete'){throw}
}
if(Test-Path $receipt){throw 'receipt published before state'}
Save $state @{status='passed';restored=$true;scenario='running'}
$published=PublishReceipt $state $receipt @{rows=@()}
if($published.state_sha256 -cne (Get-FileHash $state -Algorithm SHA256).Hash.ToLowerInvariant()){throw 'receipt not bound to state'}
""")

    def test_upgrade_scenarios_use_actual_predecessor_and_transaction_order(self):
        source = str(ROOT / 'packaging/windows/Invoke-UpgradeRehearsal.ps1').replace("'", "''")
        scratch = str(self.root).replace("'", "''")
        self.run_powershell(r"""
$ErrorActionPreference='Stop'
$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile('""" + source + r"""',[ref]$tokens,[ref]$errors)
foreach($function in $ast.FindAll({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst]},$true)) {
 if($function.Name -in @('InvokeScenario','Require')){Invoke-Expression $function.Extent.Text}
}
$stateRoot='""" + scratch + r"""'
Set-Content (Join-Path $stateRoot 'Control-Release.ps1') 'param($Action,$StateRoot)'
$settings=@{incumbent=@{package_sha256='old'};candidate=@{package_sha256='new'}}
function Observe {return $script:active}
function Journals {return $script:journals}
function ReadJson($Path) {
 if($Path -like '*failure-receipt.json'){return @{failure='release listen port is already owned'}}
 return @{status='repaired';phase='repair_completed'}
}
function Absent($Journal) {$script:absent++}
function Install($Release,$Mode,$Tag) {
 $script:calls+=($Release+'/'+$Mode)
 if($Release -eq 'incumbent' -or $Mode -eq 'repair') {
  $script:active=@{active='old-id';package='old';prior_paused=$false;owner_sha256='owner'}
  return 0
 }
 if($Mode -eq 'fault'){
  $script:journals=@(@{status='rolled_back';phase='rollback_completed';journal_path=(Join-Path $stateRoot 'journal.json')})
  return 1
 }
 if($Mode -eq 'interrupt'){
  $script:journals=@(@{status='in_progress';instance_id='new-id';journal_path='pending'})
  return 1
 }
 $script:active=@{active='new-id';package='new';previous='old-id';prepared=$null
  effective_weights=@{converted_device_bytes=1;host_mapped_bytes=1}}
 $script:journals+=@{status='completed';phase='completed';task_was_running=$true;flags=@{incumbent_touched=$true}}
 return 0
}
foreach($Scenario in @('running','rollback','repair')){
 $script:calls=@();$script:journals=@();$script:absent=0
 InvokeScenario
 $expected=switch($Scenario){
  'running' {'incumbent/install,candidate/install'}
  'rollback' {'incumbent/install,candidate/fault'}
  'repair' {'incumbent/install,candidate/interrupt,candidate/repair,candidate/install'}
 }
 if(($script:calls -join ',') -cne $expected){throw 'scenario order differs'}
 if($Scenario -ne 'running' -and $script:absent -ne 1){throw 'candidate cleanup not checked'}
}
""")


if __name__ == '__main__':
    unittest.main()
