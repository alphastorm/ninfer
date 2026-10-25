import importlib.util
import json
import tempfile
import unittest
from pathlib import Path


SPEC = importlib.util.spec_from_file_location(
    "exp097", Path(__file__).resolve().parents[1] / "tools/bench/run_exp097.py")
bench = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(bench)


class Exp097AccountingTests(unittest.TestCase):
    def test_weighted_decode_is_not_mean_of_rates(self):
        rows = [
            {"http_status": 200, "completion_tokens": 100,
             "server": {"decode": 1, "rounds": 20, "decode_tok_s": 100}},
            {"http_status": 200, "completion_tokens": 900,
             "server": {"decode": 3, "rounds": 100, "decode_tok_s": 300}},
            {"http_status": 500},
        ]
        result = bench.aggregate(rows)
        self.assertEqual(result["attempted"], 3)
        self.assertEqual(result["completed"], 2)
        self.assertEqual(result["weighted_decode_tok_s"], 250)
        self.assertEqual(result["median_decode_tok_s"], 200)
        self.assertAlmostEqual(result["ms_per_round"], 1000 / 30)
        self.assertEqual(result["p95_decode_ms_per_token"], 10)

    def test_identical_token_shapes_consume_distinct_records(self):
        def event(when, decode):
            return {"event": "request_done", "timestamp_unix_ms": when,
                    "result": {"prompt_tokens": 20, "completion_tokens": 100},
                    "timings_seconds": {"decode": decode}, "speculative": {"rounds": 25}}
        rows = [{"id": str(i), "http_status": 200, "prompt_tokens": 20,
                 "completion_tokens": 100, "submitted_unix_ms": 200} for i in range(2)]
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "requests.jsonl"
            log.write_text("\n".join(json.dumps(r) for r in
                                    (event(100, 9), event(210, 1), event(220, 2))) + "\n{")
            bench.bind_server(log, rows, 150)
        self.assertEqual([r["server"]["decode"] for r in rows], [1, 2])
        self.assertEqual([r["server"]["ms_per_round"] for r in rows], [40, 80])

    def test_missing_log_is_failure_not_zero_throughput(self):
        rows = [{"id": "case", "http_status": 200, "prompt_tokens": 20,
                 "completion_tokens": 100, "submitted_unix_ms": 200}]
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "requests.jsonl"
            log.write_text("")
            with self.assertRaisesRegex(RuntimeError, "missing request_done"):
                bench.bind_server(log, rows, 150)

    def test_failed_requests_are_not_matched_to_successes(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "requests.jsonl"
            log.write_text("")
            rows = [{"id": "failed", "http_status": 400}]
            bench.bind_server(log, rows, 0)
            self.assertEqual(bench.aggregate(rows), {"completed": 0, "attempted": 1})


if __name__ == "__main__":
    unittest.main()
