"""GPU-free regressions distilled from the private release wrappers."""
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


class PrivateWrapperShapes:
    """Sanitized pre-promotion behavior; replaced by repository drivers at green."""

    @staticmethod
    def claim_attempt(parent, attempt):
        path = parent / attempt
        path.mkdir(parents=True, exist_ok=True)
        return path

    @staticmethod
    def parse_object(text, *, compact=False):
        for line in reversed(text.splitlines()):
            if line.strip().startswith('{') and line.strip().endswith('}'):
                return json.loads(line)
        raise ValueError('command emitted no compact JSON receipt')

    @staticmethod
    def require_census(value):
        return value['rows']

    @staticmethod
    def require_disjoint_logs(logs, roots):
        return None

    @staticmethod
    def atomic_json(path, value):
        path.write_text(json.dumps(value))

    @classmethod
    def publish_completed(cls, state_path, receipt_path, state, receipt):
        cls.atomic_json(receipt_path, receipt)
        state['status'] = 'passed'
        cls.atomic_json(state_path, state)

    @staticmethod
    def completed_receipt(state_path, receipt_path):
        return json.loads(receipt_path.read_text())


driver = PrivateWrapperShapes


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


if __name__ == '__main__':
    unittest.main()
