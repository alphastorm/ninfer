#!/usr/bin/env python3
"""Summarize exact M=1 versus M=4 Nsight Compute captures for Q4 LinearSwiGLU."""

from __future__ import annotations

import argparse
import csv
import json
import math
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


class SummaryError(ValueError):
    pass


@dataclass(frozen=True)
class Metric:
    value: float
    unit: str


_DURATION_METRICS = ("gpu__time_duration.sum", "gpu__time_duration.avg")
_DRAM_RATE_METRICS = ("dram__bytes.sum.per_second",)
_OPTIONAL_METRICS = (
    "launch__registers_per_thread",
    "launch__shared_mem_per_block_static",
    "launch__shared_mem_per_block_dynamic",
    "launch__block_size",
    "launch__grid_size",
    "sm__warps_active.avg.pct_of_peak_sustained_active",
    "lts__t_bytes.sum",
)


def _number(raw: str, *, path: Path, metric: str) -> float:
    try:
        value = float(raw.strip().replace(",", ""))
    except ValueError as error:
        raise SummaryError(f"{path}: invalid value for {metric}: {raw!r}") from error
    if not math.isfinite(value):
        raise SummaryError(f"{path}: non-finite value for {metric}: {raw!r}")
    return value


def _rows(path: Path) -> tuple[list[str], list[str], list[list[str]]]:
    with path.open(newline="", encoding="utf-8-sig") as stream:
        parsed = list(csv.reader(stream))
    for index, row in enumerate(parsed):
        if "ID" in row and "Kernel Name" in row and any(name in row for name in _DURATION_METRICS):
            if index + 1 >= len(parsed) or len(parsed[index + 1]) != len(row):
                raise SummaryError(f"{path}: Nsight Compute CSV unit row is missing or malformed")
            data = [candidate for candidate in parsed[index + 2 :] if any(candidate)]
            if any(len(candidate) != len(row) for candidate in data):
                raise SummaryError(f"{path}: Nsight Compute CSV data row is malformed")
            return row, parsed[index + 1], data
    raise SummaryError(f"{path}: Nsight Compute wide CSV header was not found")


def _read_metrics(path: Path) -> tuple[str, dict[str, Metric]]:
    header, units, rows = _rows(path)
    positions = {name: index for index, name in enumerate(header)}
    if len(rows) != 1:
        launches = ", ".join(
            f"{row[positions['ID']]}:{row[positions['Kernel Name']]}" for row in rows
        )
        raise SummaryError(f"{path}: expected one profiled kernel launch, found {launches}")

    row = rows[0]
    requested = set(_DURATION_METRICS + _DRAM_RATE_METRICS + _OPTIONAL_METRICS)
    metrics = {
        name: Metric(_number(row[index], path=path, metric=name), units[index].strip())
        for name, index in positions.items()
        if name in requested and row[index].strip()
    }
    if not metrics:
        raise SummaryError(f"{path}: no Nsight Compute metrics were found")
    return row[positions["Kernel Name"]], metrics


def _select(metrics: dict[str, Metric], names: Iterable[str], *, path: Path) -> Metric:
    for name in names:
        if name in metrics:
            return metrics[name]
    raise SummaryError(f"{path}: required metric is missing: {' or '.join(names)}")


def _duration_us(metric: Metric, *, path: Path) -> float:
    factors = {
        "nsecond": 1.0e-3,
        "ns": 1.0e-3,
        "usecond": 1.0,
        "us": 1.0,
        "msecond": 1.0e3,
        "ms": 1.0e3,
        "second": 1.0e6,
        "s": 1.0e6,
    }
    try:
        return metric.value * factors[metric.unit]
    except KeyError as error:
        raise SummaryError(f"{path}: unsupported duration unit: {metric.unit!r}") from error


def _bytes_per_second(metric: Metric, *, path: Path) -> float:
    factors = {
        "byte/s": 1.0,
        "Kbyte/s": 1.0e3,
        "Mbyte/s": 1.0e6,
        "Gbyte/s": 1.0e9,
        "Tbyte/s": 1.0e12,
        "KiB/s": 1024.0,
        "MiB/s": 1024.0**2,
        "GiB/s": 1024.0**3,
        "TiB/s": 1024.0**4,
    }
    try:
        return metric.value * factors[metric.unit]
    except KeyError as error:
        raise SummaryError(f"{path}: unsupported byte-rate unit: {metric.unit!r}") from error


def read_capture(path: Path) -> dict[str, object]:
    kernel, metrics = _read_metrics(path)
    duration = _duration_us(_select(metrics, _DURATION_METRICS, path=path), path=path)
    dram_rate = _bytes_per_second(_select(metrics, _DRAM_RATE_METRICS, path=path), path=path)
    dram_bytes = dram_rate * duration * 1.0e-6
    if duration <= 0.0 or dram_bytes <= 0.0:
        raise SummaryError(f"{path}: duration and DRAM bytes must be positive")

    optional = {
        name: {"value": metrics[name].value, "unit": metrics[name].unit}
        for name in _OPTIONAL_METRICS
        if name in metrics
    }
    return {
        "source": str(path),
        "kernel": kernel,
        "duration_us": duration,
        "dram_bytes": dram_bytes,
        "effective_dram_gb_s": dram_bytes / duration / 1.0e3,
        "optional_metrics": optional,
    }


def read_shape_manifest(path: Path) -> dict[int, dict[str, object]]:
    records: dict[int, dict[str, object]] = {}
    expected_geometry = {
        "gate_up_n": 34816,
        "output_n": 17408,
        "k": 5120,
        "group_size": 64,
    }
    for line_number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not raw.strip():
            continue
        try:
            record = json.loads(raw)
        except json.JSONDecodeError as error:
            raise SummaryError(f"{path}:{line_number}: invalid JSON") from error
        if (
            record.get("schema_version") != 1
            or record.get("kernel_family") != "linear_swiglu_q4"
            or record.get("route") != "public"
        ):
            raise SummaryError(f"{path}:{line_number}: manifest is not the public Q4 LinearSwiGLU route")
        if any(record.get(name) != value for name, value in expected_geometry.items()):
            raise SummaryError(f"{path}:{line_number}: manifest has the wrong Q4 LinearSwiGLU geometry")
        if record.get("gate_up_paired") is not True or record.get("silu_epilogue_fused") is not True:
            raise SummaryError(f"{path}:{line_number}: manifest is not the paired fused route")
        m = record.get("m")
        if not isinstance(m, int) or m in records:
            raise SummaryError(f"{path}:{line_number}: invalid or duplicate m")
        records[m] = record

    if set(records) != {1, 4}:
        raise SummaryError(f"{path}: manifest must contain exactly m=1 and m=4")
    if records[1].get("schedule") != "linear_swiglu.q4.gemv.paired_rows":
        raise SummaryError(f"{path}: m=1 does not resolve to the paired-row GEMV route")
    m4 = records[4]
    if m4.get("schedule") != "linear_swiglu.q4.mma.small_t.exact":
        raise SummaryError(f"{path}: m=4 does not resolve to the exact small-T route")
    if m4.get("weights_reused_across_m_within_cta") is not True:
        raise SummaryError(f"{path}: m=4 manifest does not declare intra-CTA weight reuse")
    return records


def summarize(m1_csv: Path, m4_csv: Path, manifest_path: Path) -> dict[str, object]:
    manifest = read_shape_manifest(manifest_path)
    captures = {1: read_capture(m1_csv), 4: read_capture(m4_csv)}
    expected_kernels = {
        1: "q4_linear_swiglu_gemv_pair_kernel",
        4: "q4_small_t_mma_kernel",
    }
    for m, expected in expected_kernels.items():
        if expected not in str(captures[m]["kernel"]):
            raise SummaryError(f"m={m} capture is not the expected {expected} launch")
    duration_ratio = float(captures[4]["duration_us"]) / float(captures[1]["duration_us"])
    dram_ratio = float(captures[4]["dram_bytes"]) / float(captures[1]["dram_bytes"])
    return {
        "schema_version": 1,
        "contract": {
            "draft_tokens": 3,
            "target_verify_m": 4,
            "comparison_m": 1,
            "ratio_definition": "m4_over_m1",
        },
        "shape_manifest": [manifest[1], manifest[4]],
        "captures": {"m1": captures[1], "m4": captures[4]},
        "ratios": {
            "dram_bytes": dram_ratio,
            "kernel_duration": duration_ratio,
        },
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--m1-csv", type=Path, required=True)
    parser.add_argument("--m4-csv", type=Path, required=True)
    parser.add_argument("--shape-manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        result = summarize(args.m1_csv, args.m4_csv, args.shape_manifest)
    except (OSError, SummaryError) as error:
        raise SystemExit(f"summarize_ncu_q4: {error}") from error
    rendered = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if args.output is None:
        print(rendered, end="")
    else:
        args.output.write_text(rendered, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
