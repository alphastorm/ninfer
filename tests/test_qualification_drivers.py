"""GPU-free regressions distilled from the private release wrappers."""
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import sys
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools.qualification import driver_state as driver
from tools.qualification import run_native_chain as chain


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

if __name__ == '__main__':
    unittest.main()
