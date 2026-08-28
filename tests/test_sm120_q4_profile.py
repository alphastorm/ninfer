from __future__ import annotations

import csv
import json
import tempfile
import unittest
from pathlib import Path

from tools.bench.summarize_ncu_q4 import SummaryError, read_capture, summarize


def write_capture(
    path: Path,
    *,
    launch_id: int,
    kernel: str,
    duration_us: float,
    read_mbytes: float,
    write_mbytes: float,
) -> None:
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(
            [
                "ID",
                "Kernel Name",
                "gpu__time_duration.sum",
                "dram__bytes.sum.per_second",
                "launch__registers_per_thread",
            ]
        )
        writer.writerow(["", "", "us", "Gbyte/s", "register/thread"])
        dram_gb_s = (read_mbytes + write_mbytes) / duration_us * 1.0e3
        writer.writerow([launch_id, kernel, duration_us, dram_gb_s, 96])


def write_manifest(path: Path, *, m4_schedule: str = "linear_swiglu.q4.mma.small_t.exact") -> None:
    shared = {
        "schema_version": 1,
        "kernel_family": "linear_swiglu_q4",
        "gate_up_n": 34816,
        "output_n": 17408,
        "k": 5120,
        "group_size": 64,
        "route": "public",
        "gate_up_paired": True,
        "silu_epilogue_fused": True,
    }
    records = (
        {
            **shared,
            "m": 1,
            "schedule": "linear_swiglu.q4.gemv.paired_rows",
            "weights_reused_across_m_within_cta": None,
        },
        {
            **shared,
            "m": 4,
            "schedule": m4_schedule,
            "weights_reused_across_m_within_cta": True,
        },
    )
    path.write_text("".join(json.dumps(record) + "\n" for record in records), encoding="utf-8")


class Sm120Q4ProfileTest(unittest.TestCase):
    def test_summarizes_exact_mtp3_width_ratios(self) -> None:
        with tempfile.TemporaryDirectory() as raw_tmp:
            tmp = Path(raw_tmp)
            m1 = tmp / "m1.csv"
            m4 = tmp / "m4.csv"
            manifest = tmp / "shape.jsonl"
            write_capture(
                m1,
                launch_id=1,
                kernel="q4_linear_swiglu_gemv_pair_kernel",
                duration_us=100.0,
                read_mbytes=100.0,
                write_mbytes=5.0,
            )
            write_capture(
                m4,
                launch_id=2,
                kernel="q4_small_t_mma_kernel",
                duration_us=130.0,
                read_mbytes=105.0,
                write_mbytes=6.0,
            )
            write_manifest(manifest)

            result = summarize(m1, m4, manifest)

            self.assertEqual(result["contract"]["draft_tokens"], 3)
            self.assertEqual(result["contract"]["target_verify_m"], 4)
            self.assertAlmostEqual(result["ratios"]["kernel_duration"], 1.3)
            self.assertAlmostEqual(result["ratios"]["dram_bytes"], 111.0 / 105.0)
            self.assertAlmostEqual(result["captures"]["m4"]["effective_dram_gb_s"], 111.0 / 0.13)

    def test_rejects_capture_with_multiple_kernel_launches(self) -> None:
        with tempfile.TemporaryDirectory() as raw_tmp:
            path = Path(raw_tmp) / "capture.csv"
            write_capture(
                path,
                launch_id=1,
                kernel="q4_small_t_mma_kernel",
                duration_us=130.0,
                read_mbytes=105.0,
                write_mbytes=6.0,
            )
            with path.open("a", newline="", encoding="utf-8") as stream:
                csv.writer(stream).writerow([2, "unexpected_kernel", 1.0, 1.0, 96])

            with self.assertRaisesRegex(SummaryError, "expected one profiled kernel launch"):
                read_capture(path)

    def test_rejects_manifest_when_m4_is_not_exact_small_t(self) -> None:
        with tempfile.TemporaryDirectory() as raw_tmp:
            tmp = Path(raw_tmp)
            m1 = tmp / "m1.csv"
            m4 = tmp / "m4.csv"
            manifest = tmp / "shape.jsonl"
            write_capture(
                m1,
                launch_id=1,
                kernel="q4_linear_swiglu_gemv_pair_kernel",
                duration_us=100.0,
                read_mbytes=100.0,
                write_mbytes=5.0,
            )
            write_capture(
                m4,
                launch_id=2,
                kernel="q4_small_t_mma_kernel",
                duration_us=130.0,
                read_mbytes=105.0,
                write_mbytes=6.0,
            )
            write_manifest(manifest, m4_schedule="linear_swiglu.q4.materialized")

            with self.assertRaisesRegex(SummaryError, "m=4 does not resolve"):
                summarize(m1, m4, manifest)

    def test_rejects_capture_when_kernel_does_not_match_width(self) -> None:
        with tempfile.TemporaryDirectory() as raw_tmp:
            tmp = Path(raw_tmp)
            m1 = tmp / "m1.csv"
            m4 = tmp / "m4.csv"
            manifest = tmp / "shape.jsonl"
            write_capture(
                m1,
                launch_id=1,
                kernel="q4_small_t_mma_kernel",
                duration_us=100.0,
                read_mbytes=100.0,
                write_mbytes=5.0,
            )
            write_capture(
                m4,
                launch_id=2,
                kernel="q4_small_t_mma_kernel",
                duration_us=130.0,
                read_mbytes=105.0,
                write_mbytes=6.0,
            )
            write_manifest(manifest)

            with self.assertRaisesRegex(SummaryError, "m=1 capture is not the expected"):
                summarize(m1, m4, manifest)


if __name__ == "__main__":
    unittest.main()
