#!/usr/bin/env python3
"""Host-only provenance and failure-preservation checks for the production restart driver."""
from __future__ import annotations

import importlib.util
from pathlib import Path
import unittest
import test_serve_shutdown_capacity_real as capacity_runner

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

    def test_capacity_includes_kv_but_not_index(self):
        a = {"native_bytes": 1024, "state_bytes": 768, "index_bytes": 99999}
        b = {"native_bytes": 1280, "state_bytes": 1024, "index_bytes": 99999}
        continuation = {"native_bytes": 1536}
        self.assertEqual(capacity_runner.constrained_capacity(a, b, continuation), 1536)

    def test_unconstructible_fixture_fails(self):
        with self.assertRaisesRegex(RuntimeError, "cannot fit"):
            capacity_runner.constrained_capacity(
                {"native_bytes": 4096, "state_bytes": 512},
                {"native_bytes": 4096, "state_bytes": 512}, {"native_bytes": 4096})

    def test_pressure_requires_actual_occupancy(self):
        a = {"states": 3, "state_bytes": 768}
        b = {"states": 4, "state_bytes": 1024}
        metrics = dict(zip(capacity_runner.METRICS, (1536, 768, 0, 3, 0, 4, 8)))
        self.assertEqual(capacity_runner.pressure_proof(metrics, a, b, 1536), 256)
        for key in ("host_context_reserved_bytes", "host_kv_used_bytes",
                    "host_context_used_bytes", "device_state_used_slots"):
            with self.subTest(key=key), self.assertRaisesRegex(RuntimeError, "invalidates"):
                capacity_runner.pressure_proof({**metrics, key: metrics[key] + 1}, a, b, 1536)

    def test_capacity_metrics_must_be_complete(self):
        text = "\n".join(f"ninfer_{name} 256" for name in capacity_runner.METRICS)
        self.assertEqual(len(capacity_runner.read_metrics(text)), len(capacity_runner.METRICS))
        with self.assertRaisesRegex(RuntimeError, "absent"):
            capacity_runner.read_metrics("")


if __name__ == "__main__":
    unittest.main()
