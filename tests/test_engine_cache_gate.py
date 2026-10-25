"""Exercise the startup gate against numeric receipts, not source-text wiring."""
from __future__ import annotations

import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("engine_cache_gate", ROOT / "tools/smoke/engine_cache_gate.py")
assert SPEC is not None and SPEC.loader is not None
GATE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GATE)
UUID = "GPU-00112233-4455-6677-8899-aabbccddeeff"


def startup() -> dict:
    return {"event": "server_start", "artifact": {"path": "/private/model.ninfer"},
            "engine": {**GATE.SHAPE, "kv_capacity_mode": "auto", "kv_capacity": 140000,
                       "context_cache": {"device_state_slots": 2}},
            "environment": {"gpu_uuid": UUID, "total_device_memory_bytes": 32607 * GATE.MIB,
                            "gpu_name": "RTX 5090"},
            "memory": {"device_budget_bytes": 32000 * GATE.MIB,
                       "runtime_reservation_bytes": 12000 * GATE.MIB,
                       "cuda_graph_rebuild_estimate_bytes": 1024 * GATE.MIB,
                       "cuda_graph_calibration_increment_bytes": 30 * GATE.MIB,
                       "cuda_graph_calibration_retained_bytes": 30 * GATE.MIB,
                       "startup_retained_bytes": 30 * GATE.MIB + 4096,
                       "available_after_startup_bytes": 1024 * GATE.MIB}}


class StartupGateTests(unittest.TestCase):
    def test_accepts_attested_shape_and_only_exports_allowlisted_fields(self) -> None:
        source = startup()
        summary = GATE.validate_startup(source, source, UUID)
        self.assertEqual(summary["engine"]["max_context"], 131072)
        self.assertNotIn("artifact", summary)
        self.assertNotIn("gpu_uuid", summary)
        self.assertNotIn("/private/", json.dumps(summary))

    def test_small_graph_growth_with_improved_headroom_passes(self) -> None:
        baseline = startup()
        current = copy.deepcopy(baseline)
        for field in GATE.GRAPH_FIELDS[1:]:
            current["memory"][field] += 2 * GATE.MIB
        current["memory"]["available_after_startup_bytes"] += 2 * GATE.MIB
        summary = GATE.validate_startup(current, baseline, UUID)
        self.assertEqual(summary["memory"]["cuda_graph_calibration_retained_bytes"],
                         32 * GATE.MIB)
        self.assertEqual(summary["startup_gate"]["graph_deltas_bytes"]
                         ["cuda_graph_calibration_retained_bytes"], 2 * GATE.MIB)

    def test_gib_graph_growth_fails_even_if_the_server_is_healthy(self) -> None:
        baseline = startup()
        for field in GATE.GRAPH_FIELDS:
            with self.subTest(field=field):
                current = copy.deepcopy(baseline)
                current["memory"][field] += 1024 * GATE.MIB
                with self.assertRaisesRegex(GATE.GateError, "cuda_graph_reservation_growth"):
                    GATE.validate_startup(current, baseline, UUID)

    def test_absolute_and_baseline_headroom_floors(self) -> None:
        baseline = startup()
        for headroom in (767 * GATE.MIB, baseline["memory"]["available_after_startup_bytes"]
                        - 33 * GATE.MIB):
            current = copy.deepcopy(baseline)
            current["memory"]["available_after_startup_bytes"] = headroom
            with self.subTest(headroom=headroom), self.assertRaisesRegex(
                    GATE.GateError, "insufficient_startup_headroom"):
                GATE.validate_startup(current, baseline, UUID)
        current["memory"]["available_after_startup_bytes"] = 992 * GATE.MIB
        GATE.validate_startup(current, baseline, UUID)
        baseline["memory"]["available_after_startup_bytes"] = 800 * GATE.MIB
        current["memory"]["available_after_startup_bytes"] = 768 * GATE.MIB
        GATE.validate_startup(current, baseline, UUID)

    def test_zero_legacy_estimate_uses_real_retained_calibration(self) -> None:
        baseline = startup()
        baseline["memory"]["cuda_graph_rebuild_estimate_bytes"] = 0
        summary = GATE.validate_startup(baseline, baseline, UUID)
        self.assertEqual(summary["memory"]["cuda_graph_rebuild_estimate_bytes"], 0)
        self.assertEqual(summary["memory"]["cuda_graph_calibration_retained_bytes"],
                         30 * GATE.MIB)

    def test_refused_context_or_wrong_shape_does_not_pass(self) -> None:
        baseline = startup()
        for field, value in (("max_concurrency", 1), ("max_context", 32768),
                             ("speculative_draft_window", 3), ("vision", False),
                             ("cuda_graph", False), ("kv_cache", "fp8")):
            with self.subTest(field=field):
                current = copy.deepcopy(baseline)
                current["engine"][field] = value
                with self.assertRaises(GATE.GateError):
                    GATE.validate_startup(current, baseline, UUID)
        current = copy.deepcopy(baseline)
        current["engine"]["kv_capacity"] = 65536
        with self.assertRaisesRegex(GATE.GateError, "logical_context_refused"):
            GATE.validate_startup(current, baseline, UUID)

    def test_device_attestation_and_budget_are_not_optional(self) -> None:
        baseline = startup()
        for field, value in (("gpu_uuid", "other"), ("total_device_memory_bytes", 48 << 30)):
            current = copy.deepcopy(baseline)
            current["environment"][field] = value
            with self.subTest(field=field), self.assertRaises(GATE.GateError):
                GATE.validate_startup(current, baseline, UUID)
        current = copy.deepcopy(baseline)
        current["memory"]["runtime_reservation_bytes"] = 33 << 30
        with self.assertRaisesRegex(GATE.GateError, "runtime_reservation_exceeds_device_budget"):
            GATE.validate_startup(current, baseline, UUID)

    def test_requires_exactly_one_real_startup_record(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "events.jsonl"
            path.write_text(json.dumps({"event": "request_done"}) + "\n")
            with self.assertRaises(GATE.GateError):
                GATE.startup_record(path)
            path.write_text(json.dumps(startup()) + "\n")
            self.assertEqual(GATE.startup_record(path)["event"], "server_start")
            path.write_text((json.dumps(startup()) + "\n") * 2)
            with self.assertRaises(GATE.GateError):
                GATE.startup_record(path)


if __name__ == "__main__":
    unittest.main()
