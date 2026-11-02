"""GPU-free regressions distilled from the private release wrappers."""
import dataclasses
import hashlib
import json
import ntpath
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

    def test_windows_drive_roots_overlap_in_both_directions(self):
        for logs, root in (('Q:/logs', 'q:/'), ('Q:/', 'q:/inputs'),
                           ('Q:/', 'q:\\'), ('Q:/logs', 'Q:/logs/inputs')):
            with self.subTest(logs=logs, root=root), self.assertRaises(ValueError):
                driver.require_disjoint_logs(logs, [root])
        driver.require_disjoint_logs('Q:/logs', ['R:/'])
        driver.require_disjoint_logs('Q:/inputs-other', ['Q:/inputs'])

    def test_windows_relative_paths_use_one_case_insensitive_absolute_namespace(self):
        with patch.object(driver, 'os', SimpleNamespace(name='nt')), \
                patch.object(ntpath, 'abspath', side_effect=lambda value: ntpath.normpath(
                    ntpath.join('Q:/Workspace', value))):
            for logs, root in (('logs/output', 'q:/WORKSPACE/LOGS'),
                               ('Q:/Workspace/Inputs/log', 'inputs'),
                               ('./Logs/../Inputs', 'q:/workspace/INPUTS'),
                               ('logs', 'Q:/Workspace/LOGS/inputs')):
                with self.subTest(logs=logs, root=root), self.assertRaises(ValueError):
                    driver.require_disjoint_logs(logs, [root])
            driver.require_disjoint_logs('logs', ['Q:/Workspace/inputs'])

    @unittest.skipIf(driver.os.name == 'nt', 'POSIX filesystem-root semantics')
    def test_posix_filesystem_root_overlaps_descendants(self):
        with self.assertRaises(ValueError):
            driver.require_disjoint_logs(str(self.root), ['/'])

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
            self.assertNotEqual(json.loads(path.read_text())['status'], 'passed')
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
$settings=@{incumbent=@{package_sha256='old'};candidate=@{package_sha256='new';instance_id='new-id'}}
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


class NativeChainBoundaryTests(unittest.TestCase):
    """Use the real constructor, state loader, phase publisher, preflight and cleanup."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        fixture = self.root / 'fixture.json'
        fixture.write_text('[]', encoding='utf-8')
        lane_root = ROOT / 'packaging/windows/lanes/rtx4090'
        self.lane = chain.native.Lane(
            'rtx4090', json.loads((lane_root / 'release-spec.json').read_text()),
            json.loads((lane_root / 'server-config.json').read_text()))
        self.scratch_root = 'Q:/work/{attempt}-target/' + self.lane.spec['lifecycle']['state_root_name']
        self.values = {field.name: '' for field in dataclasses.fields(chain.native.Config)}
        self.values.update(
            source_root=str(self.root / 'source'), long_fixture=str(fixture), transfer_tool=None,
            lane=self.lane.name, builder='builder', target='target', incumbent_state_root=None,
            candidate_source='a' * 40, scratch_state_root=self.scratch_root,
            scratch_port=18200, pressure_port=18201, api_key_file='R:/inputs/key',
            qualified_runtime_bin=None, qualified_build_receipt=None, gpu_uuid=None)
        self.settings = {
            'evidence_parent': str(self.root / 'outputs'), 'builder_parent': 'Q:/work',
            'target_parent': 'Q:/work', 'native': self.values, 'expected_package_sha256': 'package'}
        self.config_path = self.root / 'config.json'
        self.calls = []
        self.instances = []
        self.host = {'incumbent_running': False, 'candidate_running': False, 'installed': False}
        case = self

        class LifecycleOrchestrator(chain.ChainOrchestrator):
            def __init__(self, config):
                super().__init__(config)
                self.loaded = json.loads(json.dumps(self.state))
                case.instances.append(self)

            @property
            def gpu_uuid(self):
                return self.state.setdefault('gpu_uuid', 'GPU-01234567-89ab-cdef-0123-456789abcdef')

            def remote_json(self, host, script, timeout=600):
                if '$ompModels=' in script:
                    return {'incumbent_state_root': self.config.incumbent_state_root or '',
                            'incumbent_was_running': case.host['incumbent_running'],
                            'omp_models': ['ninfer-client-acceptance/' + case.lane.model_id]}
                if '$modelPath=' in script:
                    case.host.update(incumbent_running=False, candidate_running=False)
                    return {'lineage_exists': case.host['installed'],
                            'model_sha256': case.lane.model_sha256,
                            'power_limit_w': case.lane.owner_power_limit_w,
                            'gpu': case.lane.spec['gpu']['name']}
                if "status='stop-observed'" in script:
                    case.host['candidate_running'] = False
                    return {'status': 'stop-observed', 'held_launch_ids': []}
                if "$incumbentState='untouched'" in script:
                    case.host['candidate_running'] = False
                    case.host['incumbent_running'] = bool(
                        self.state.get('found_incumbent', {}).get('incumbent_was_running'))
                    return {'status': 'passed', 'process_count': 0,
                            'power_limit_w': case.lane.owner_power_limit_w}
                if 'scratch_retired' in script:
                    return {'status': 'passed', 'scratch_retired': True}
                if 'attempt namespace already exists' in script:
                    return {'status': 'passed'}
                raise AssertionError('unexpected remote provider call')

        self.orchestrator = LifecycleOrchestrator
        def run(args, **kwargs):
            output = ''
            if args[:2] == ['git', 'rev-parse']:
                output = 'a' * 40
            elif args[:2] == ['git', 'show']:
                # Supply actual repository bytes to the real Lane.load and protocol
                # preflight; new required schema fields propagate to every boundary test.
                output = (ROOT / args[2].split(':', 1)[1]).read_text()
            return SimpleNamespace(stdout=output)
        for target, name, value in (
                (chain, 'ChainOrchestrator', LifecycleOrchestrator),
                (chain.native, 'run', run),
                (chain.native, 'stage_lane_qualification', lambda *args: {'status': 'passed'})):
            mock = patch.object(target, name, value)
            mock.start()
            self.addCleanup(mock.stop)
        for name in chain.native.PHASES:
            if name in ('preflight', 'restore'):
                continue
            def provider(subject, name=name):
                case.calls.append(name)
                case.assertFalse(case.host['incumbent_running'], 'successor did not quiesce incumbent')
                receipt = {'status': 'passed'}
                if name == 'package':
                    receipt['package'] = {'sha256': 'package'}
                if name == 'transfer_install':
                    case.host['installed'] = True
                if name in ('pressure_protocol', 'security'):
                    case.host['candidate_running'] = False
                if name in ('context_128k', 'restart', 'omp', 'protocol'):
                    case.assertTrue(case.host['installed'])
                    case.host['candidate_running'] = True
                if name == 'receipt':
                    receipt['status'] = 'restore-pending'
                    driver.atomic_json(subject.config.state_dir / 'qualification-summary.json', receipt)
                return receipt
            mock = patch.object(LifecycleOrchestrator, chain.METHODS.get(name, name), provider)
            mock.start()
            self.addCleanup(mock.stop)

    def run_part(self, part, attempt='attempt'):
        driver.atomic_json(self.config_path, self.settings)
        chain.run_part(self.config_path, attempt, part)

    def stage_state(self, part, attempt='attempt'):
        return json.loads((self.root / 'outputs' / attempt / (part + '.state.json')).read_text())

    def test_every_mutable_root_rejects_every_immutable_input_before_construction(self):
        inputs = (
            'source_root', 'long_fixture', 'transfer_tool', 'builder_vcpkg', 'builder_cuda_compiler',
            'builder_cxx_compiler', 'model_path', 'omp_root', 'incumbent_state_root',
            'dependency_sbom_tool', 'vcpkg_resolved', 'cuda_inventory', 'third_party_inventory',
            'runtime_inventory', 'api_key_file', 'qualified_runtime_bin', 'qualified_build_receipt')
        original = dict(self.values)
        with patch.object(chain, 'ChainOrchestrator', side_effect=AssertionError(
                'overlap must be refused before orchestrator construction')) as constructor:
            for index, key in enumerate(inputs):
                for kind in ('controller', 'builder', 'target', 'scratch'):
                    attempt = f'overlap-{index}-{kind}'
                    directory = self.root / 'outputs' / attempt
                    roots = {
                        'controller': str(directory),
                        'builder': f'Q:/work/{attempt}-builder',
                        'target': f'Q:/work/{attempt}-target',
                        'scratch': self.scratch_root.replace('{attempt}', attempt)}
                    self.values.clear()
                    self.values.update(original)
                    self.values[key] = roots[kind] + '/frozen'
                    with self.subTest(input=key, mutable=kind), self.assertRaises(ValueError):
                        self.run_part('cpu', attempt)
                    self.assertFalse(directory.exists())
                    constructor.assert_not_called()

    def test_invalid_remote_roots_leave_no_attempt_artifacts(self):
        for index, (key, value) in enumerate((
                ('builder_parent', 'relative'), ('target_parent', 'Q:/work/../escape'),
                ('scratch_state_root', 'Q:/foreign/managed'))):
            with self.subTest(key=key), patch.object(chain, 'ChainOrchestrator') as constructor:
                if key in self.settings:
                    self.settings[key] = value
                else:
                    self.values[key] = value
                with self.assertRaises(ValueError):
                    self.run_part('cpu', f'invalid-{index}')
                constructor.assert_not_called()
                self.assertFalse((self.root / 'outputs' / f'invalid-{index}').exists())
                self.settings.update(builder_parent='Q:/work', target_parent='Q:/work')
                self.values['scratch_state_root'] = self.scratch_root

    def test_continuation_binds_each_imported_driver_module(self):
        self.run_part('cpu')
        original = Path.read_bytes
        for module in (driver, chain.native, chain):
            def changed(path, module=module):
                return original(path) + (b'\n# changed driver bytes\n'
                                         if path == Path(module.__file__) else b'')
            with self.subTest(module=module.__name__), patch.object(Path, 'read_bytes', changed):
                with self.assertRaisesRegex(ValueError, 'driver changed'):
                    self.run_part('protocol')
            self.assertFalse((self.root / 'outputs/attempt/protocol.started').exists())

    def test_real_state_loader_preserves_prior_groups_with_resume_false(self):
        self.run_part('cpu')
        expected = json.loads((self.root / 'outputs/attempt/native/state.json').read_text())['phases']
        self.run_part('protocol')
        subject = self.instances[-1]
        self.assertFalse(subject.config.resume)
        for name in chain.GROUPS['cpu']:
            self.assertEqual(subject.loaded['phases'][name], expected[name])
        self.assertEqual(self.calls.count('neutral_build'), 1)
        driver.require_phases(subject.state, subject.config.state_dir, chain.prior_phases('correctness'))

    def test_real_constructor_loads_complete_repository_lane_schema(self):
        self.run_part('cpu')
        lane = self.instances[-1].lane
        lane_root = ROOT / 'packaging/windows/lanes/rtx4090'
        self.assertEqual(lane.config, json.loads((lane_root / 'server-config.json').read_text()))
        self.assertEqual(lane.spec, json.loads((lane_root / 'release-spec.json').read_text()))

    def test_successor_groups_requiesce_restored_incumbent_without_measured_replay(self):
        self.values['incumbent_state_root'] = 'R:/incumbent'
        self.host['incumbent_running'] = True
        for part in chain.GROUPS:
            with self.subTest(part=part):
                self.run_part(part)
                self.assertTrue(self.host['incumbent_running'])
                self.assertFalse(self.host['candidate_running'])
        for name in self.calls:
            self.assertEqual(self.calls.count(name), 1, name)
        self.assertEqual(self.instances[-1].state['status'], 'passed')

    def test_restoration_failure_does_not_cleanup_twice(self):
        calls = []
        def fail(subject):
            calls.append('cleanup')
            subject.state['restoration'] = {'status': 'failed'}
        with patch.object(self.orchestrator, 'cleanup', fail):
            with self.assertRaisesRegex(ValueError, 'inter-stage restoration failed'):
                self.run_part('cpu')
        self.assertEqual(calls, ['cleanup'])
        self.assertEqual(self.stage_state('cpu')['status'], 'failed')

    def test_final_restore_failure_does_not_cleanup_twice(self):
        for part in list(chain.GROUPS)[:-1]:
            self.run_part(part)
        calls = []
        original = self.orchestrator.remote_json
        def fail_restore(subject, host, script, timeout=600):
            if "$incumbentState='untouched'" in script:
                calls.append('restore')
                raise RuntimeError('injected final restoration failure')
            return original(subject, host, script, timeout)
        with patch.object(self.orchestrator, 'remote_json', fail_restore):
            with self.assertRaisesRegex(RuntimeError, 'injected final restoration failure'):
                self.run_part('finish')
        self.assertEqual(calls, ['restore'])
        self.assertEqual(self.stage_state('finish')['status'], 'failed')
        self.assertEqual(self.instances[-1].state['phases']['restore']['status'], 'failed')
        self.assertEqual(self.instances[-1].state['status'], 'failed')

    def test_phase_failure_persists_failed_stage_when_cleanup_raises(self):
        def cleanup(subject):
            self.assertEqual(self.stage_state('cpu')['status'], 'failed')
            raise OSError('injected cleanup failure')
        with patch.object(self.orchestrator, 'neutral_build', side_effect=RuntimeError(
                'injected phase failure')), patch.object(self.orchestrator, 'cleanup', cleanup):
            with self.assertRaisesRegex(RuntimeError, 'injected phase failure'):
                self.run_part('cpu')
        self.assertEqual(self.stage_state('cpu')['status'], 'failed')
        self.assertEqual(self.instances[-1].state['status'], 'failed')

    def test_failed_stage_persistence_cannot_skip_cleanup_or_replace_phase_error(self):
        self.values['incumbent_state_root'] = 'R:/incumbent'
        self.host['incumbent_running'] = True
        calls = []
        original_store, original_cleanup = driver.atomic_json, self.orchestrator.cleanup
        phase_error = RuntimeError('injected phase failure')
        def persist(path, value):
            if path.name == 'cpu.state.json' and value.get('status') == 'failed':
                calls.append('failed-stage-save')
                raise OSError('injected failed-stage persistence failure')
            original_store(path, value)
        def cleanup(subject):
            calls.append('cleanup')
            original_cleanup(subject)
        with patch.object(self.orchestrator, 'neutral_build', side_effect=phase_error), \
                patch.object(driver, 'atomic_json', side_effect=persist), \
                patch.object(self.orchestrator, 'cleanup', cleanup):
            with self.assertRaises(BaseException) as raised:
                self.run_part('cpu')
        self.assertEqual(calls, ['failed-stage-save', 'cleanup'])
        self.assertIs(raised.exception, phase_error)
        self.assertIn('injected failed-stage persistence failure', '\n'.join(raised.exception.__notes__))
        self.assertTrue(self.host['incumbent_running'])
        self.assertFalse(self.host['candidate_running'])
        subject = self.instances[-1]
        self.assertEqual(subject._load_state()['status'], 'failed')
        self.assertEqual(subject.state['restoration']['status'], 'passed')

    def test_namespace_refusal_records_failure_without_cleanup(self):
        with patch.object(self.orchestrator, 'remote_json', side_effect=ValueError(
                'attempt namespace already exists')), patch.object(self.orchestrator, 'cleanup') as cleanup:
            with self.assertRaisesRegex(ValueError, 'namespace already exists'):
                self.run_part('cpu')
        cleanup.assert_not_called()
        self.assertEqual(self.stage_state('cpu')['status'], 'failed')

    def test_completion_publications_follow_persisted_native_passed_state(self):
        for part in list(chain.GROUPS)[:-1]:
            self.run_part(part)
        original_store, original_native = driver.atomic_json, chain.native.atomic_json
        calls = []
        def observe(path, value):
            if (path.name in (self.lane.name + '.json', 'finish.json') or
                    (path.name == 'finish.state.json' and value.get('status') == 'passed')):
                calls.append(path.name)
                self.assertEqual(self.instances[-1]._load_state()['status'], 'passed')
        def publish_store(path, value):
            observe(path, value)
            original_store(path, value)
        def publish_native(path, value):
            observe(path, value)
            original_native(path, value)
        with patch.object(driver, 'atomic_json', side_effect=publish_store), \
                patch.object(chain.native, 'atomic_json', side_effect=publish_native):
            self.run_part('finish')
        self.assertEqual(calls, [self.lane.name + '.json', 'finish.state.json', 'finish.json'])
        directory = self.root / 'outputs/attempt'
        self.assertEqual(driver.completed_receipt(
            directory / 'finish.state.json', directory / 'finish.json')['status'], 'passed')

    def test_native_passed_state_failure_prevents_completion_publication(self):
        for part in list(chain.GROUPS)[:-1]:
            self.run_part(part)
        original = chain.native.atomic_json
        directory = self.root / 'outputs/attempt'
        def persist(path, value):
            if path == directory / 'native/state.json' and value.get('status') == 'passed':
                raise OSError('injected native passed-state persistence failure')
            original(path, value)
        with patch.object(chain.native, 'atomic_json', side_effect=persist):
            with self.assertRaisesRegex(OSError, 'native passed-state persistence failure'):
                self.run_part('finish')
        self.assertFalse((directory / 'native' / (self.lane.name + '.json')).exists())
        self.assertFalse((directory / 'finish.json').exists())
        self.assertEqual(self.stage_state('finish')['status'], 'failed')
        self.assertEqual(self.instances[-1]._load_state()['status'], 'failed')

    def test_finish_receipt_refuses_nonpassed_native_state(self):
        for part in chain.GROUPS:
            self.run_part(part)
        subject = self.instances[-1]
        directory = self.root / 'outputs/attempt'
        def completed():
            return driver.completed_receipt(directory / 'finish.state.json', directory / 'finish.json')
        self.assertEqual(completed()['status'], 'passed')
        for status in ('not_started', 'running', 'paused', 'failed'):
            with self.subTest(status=status):
                chain.native.atomic_json(subject.state_path, {**subject.state, 'status': status})
                self.assertEqual(subject._load_state()['status'], status)
                with self.assertRaisesRegex(ValueError, 'native state'):
                    completed()
        subject.state_path.unlink()
        with self.assertRaises((ValueError, FileNotFoundError)):
            completed()
        subject._save()
        self.assertEqual(completed()['status'], 'passed')

    def test_failed_final_publication_leaves_failed_native_state(self):
        original_store, original_native = driver.atomic_json, chain.native.atomic_json
        for failure_at in ('compose', 'lane', 'stage_state', 'stage_receipt'):
            attempt = 'final-' + failure_at.replace('_', '-')
            for part in list(chain.GROUPS)[:-1]:
                self.run_part(part, attempt)
            saw = []
            def fail():
                state = json.loads((self.root / 'outputs' / attempt / 'native/state.json').read_text())
                saw.append(state['status'])
                raise OSError('injected final publication failure')
            def compose(*args):
                if failure_at == 'compose':
                    fail()
                return {'status': 'passed'}
            def publish_store(path, value):
                if ((failure_at == 'stage_receipt' and path.name == 'finish.json') or
                        (failure_at == 'stage_state' and path.name == 'finish.state.json'
                         and value.get('status') == 'passed')):
                    fail()
                original_store(path, value)
            def publish_native(path, value):
                if failure_at == 'lane' and path.name == self.lane.name + '.json':
                    fail()
                original_native(path, value)
            with self.subTest(failure_at=failure_at), \
                    patch.object(chain.native, 'stage_lane_qualification', side_effect=compose), \
                    patch.object(chain.native, 'atomic_json', side_effect=publish_native), \
                    patch.object(driver, 'atomic_json', side_effect=publish_store), \
                    patch.object(self.orchestrator, 'cleanup') as cleanup:
                with self.assertRaisesRegex(OSError, 'final publication failure'):
                    self.run_part('finish', attempt)
                cleanup.assert_not_called()
                self.assertEqual(saw, ['running' if failure_at == 'compose' else 'passed'])
                self.assertEqual(self.stage_state('finish', attempt)['status'], 'failed')
                self.assertEqual(self.instances[-1].state['status'], 'failed')

def load_tests(loader, tests, pattern):
    # Keep the public lifecycle dispatcher proofs on both release-driver operating systems.
    spec = importlib.util.spec_from_file_location(
        'windows_dispatch_contracts', ROOT / 'tests/test_windows_release.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    tests.addTests(loader.loadTestsFromTestCase(module.ManagedShutdownDispatcherTests))
    tests.addTests(loader.loadTestsFromTestCase(module.QualificationRestorationTests))
    tests.addTests(loader.loadTestsFromTestCase(module.SharedLifecycleTreeTests))
    return tests


if __name__ == '__main__':
    unittest.main()
