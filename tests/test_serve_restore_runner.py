#!/usr/bin/env python3
"""Host-only provenance and failure-preservation checks for the production restart driver."""
from __future__ import annotations

import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    "serve_restore_real", Path(__file__).with_name("test_serve_restore_real.py"))
assert spec is not None and spec.loader is not None
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class RunnerContractTest(unittest.TestCase):
    version = (f"ninfer-serve upstream_base_sha={'a' * 40} "
               f"patch_stack_sha={'b' * 40} source_dirty=false")

    def test_complete_provenance(self):
        self.assertEqual(runner.build_provenance(self.version), ("b" * 40, "a" * 40))

    def test_unknown_missing_or_malformed_upstream_base(self):
        for value in ("unknown", "", "a" * 39, "a" * 41, "A" * 40):
            with self.subTest(value=value):
                version = self.version.replace("a" * 40, value)
                with self.assertRaisesRegex(RuntimeError, "valid upstream-base provenance missing"):
                    runner.build_provenance(version)
        with self.assertRaisesRegex(RuntimeError, "valid upstream-base provenance missing"):
            runner.build_provenance(self.version.replace(f"upstream_base_sha={'a' * 40} ", ""))

    def test_dirty_source_is_refused(self):
        with self.assertRaisesRegex(RuntimeError, "clean binary source provenance missing"):
            runner.build_provenance(self.version.replace("source_dirty=false", "source_dirty=true"))

    def test_successful_cleanup_preserves_primary_error(self):
        primary = RuntimeError("startup failed")
        calls = []
        with self.assertRaises(RuntimeError) as caught:
            runner.reraise_after_stop(primary, lambda: calls.append("stop"))
        self.assertIs(caught.exception, primary)
        self.assertEqual(calls, ["stop"])

    def test_failed_cleanup_preserves_primary_and_cause(self):
        primary = RuntimeError("startup failed")
        cleanup = RuntimeError("stop failed")
        calls = []

        def stop():
            calls.append("stop")
            raise cleanup

        with self.assertRaises(RuntimeError) as caught:
            runner.reraise_after_stop(primary, stop)
        self.assertIs(caught.exception, primary)
        self.assertIs(caught.exception.__cause__, cleanup)
        self.assertEqual(calls, ["stop"])


if __name__ == "__main__":
    unittest.main()
