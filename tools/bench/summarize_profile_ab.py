#!/usr/bin/env python3
"""Summarize the fixed three-pair long-prefill/MTP3 profiling campaign."""

from __future__ import annotations

import argparse
import json
import math
import statistics
import sys
from pathlib import Path
from typing import Any, Sequence


PAIR_SUFFIXES = ("a", "b", "c")
T_CRITICAL_95_DF2 = 4.302652729911275


class SummaryError(RuntimeError):
    pass


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--result-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args(argv)


def load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise SummaryError(f"failed to load {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise SummaryError(f"{path} is not a JSON object")
    return value


def load_order(result_dir: Path) -> tuple[str, dict[str, tuple[str, str]]]:
    path = result_dir / "e2e" / "order.tsv"
    try:
        rows = [tuple(line.split("\t")) for line in path.read_text(encoding="utf-8").splitlines()]
    except OSError as exc:
        raise SummaryError(f"failed to load {path}: {exc}") from exc
    if any(len(row) != 2 for row in rows):
        raise SummaryError("order.tsv contains an invalid row")
    order = {label: (variant, label) for variant, label in rows}
    expected_labels = {
        "baseline-a",
        "candidate-a",
        "candidate-b",
        "baseline-b",
        "baseline-c",
        "candidate-c",
    }
    if set(order) != expected_labels:
        raise SummaryError(f"order.tsv labels differ from the fixed campaign: {sorted(order)}")
    candidates = {variant for variant, label in rows if label.startswith("candidate-")}
    if len(candidates) != 1 or "baseline" in candidates:
        raise SummaryError("order.tsv does not identify one candidate variant")
    return next(iter(candidates)), order


def receipt(result_dir: Path, variant: str, label: str, name: str) -> dict[str, Any]:
    record = load_json(result_dir / "runs" / f"{label}-{variant}" / name)
    summary = record.get("summary")
    if not isinstance(summary, dict):
        raise SummaryError(f"{label}/{name} has no summary object")
    return summary


def finite_float(summary: dict[str, Any], key: str, description: str) -> float:
    try:
        value = float(summary[key])
    except (KeyError, TypeError, ValueError) as exc:
        raise SummaryError(f"{description} has invalid {key}: {exc}") from exc
    if not math.isfinite(value) or value <= 0.0:
        raise SummaryError(f"{description} has nonpositive or non-finite {key}")
    return value


def metric_summary(
    baseline: dict[str, float], candidate: dict[str, float]
) -> dict[str, Any]:
    if set(baseline) != set(PAIR_SUFFIXES) or set(candidate) != set(PAIR_SUFFIXES):
        raise SummaryError("metric does not contain exactly three paired trials")
    deltas = {
        suffix: (candidate[suffix] / baseline[suffix] - 1.0) * 100.0
        for suffix in PAIR_SUFFIXES
    }
    values = list(deltas.values())
    mean = statistics.mean(values)
    stddev = statistics.stdev(values)
    half_width = T_CRITICAL_95_DF2 * stddev / math.sqrt(len(values))
    confidence_interval = [mean - half_width, mean + half_width]
    baseline_values = list(baseline.values())
    candidate_values = list(candidate.values())
    disjoint_positive = min(candidate_values) > max(baseline_values)
    disjoint_negative = max(candidate_values) < min(baseline_values)
    return {
        "baseline": baseline,
        "candidate": candidate,
        "paired_improvement_pct": deltas,
        "mean_improvement_pct": mean,
        "median_improvement_pct": statistics.median(values),
        "sample_stddev_pct": stddev,
        "paired_95pct_t_interval_pct": confidence_interval,
        "all_pairs_positive": all(value > 0.0 for value in values),
        "all_pairs_negative": all(value < 0.0 for value in values),
        "ranges_disjoint_positive": disjoint_positive,
        "ranges_disjoint_negative": disjoint_negative,
        "credible_positive": (
            all(value > 0.0 for value in values)
            and confidence_interval[0] > 0.0
            and disjoint_positive
        ),
        "credible_negative": (
            all(value < 0.0 for value in values)
            and confidence_interval[1] < 0.0
            and disjoint_negative
        ),
    }


def summarize(result_dir: Path) -> dict[str, Any]:
    candidate_variant, _ = load_order(result_dir)
    long_rates: dict[str, dict[str, float]] = {"baseline": {}, "candidate": {}}
    long_wall_rates: dict[str, dict[str, float]] = {"baseline": {}, "candidate": {}}
    mtp_rates: dict[str, dict[str, float]] = {"baseline": {}, "candidate": {}}
    hashes: dict[str, dict[str, str]] = {"long": {}, "mtp": {}}

    for suffix in PAIR_SUFFIXES:
        for role, variant, label in (
            ("baseline", "baseline", f"baseline-{suffix}"),
            ("candidate", candidate_variant, f"candidate-{suffix}"),
        ):
            long = receipt(result_dir, variant, label, "long-prefill.json")
            mtp = receipt(result_dir, variant, label, "mtp3-decode.json")
            if long.get("exact_match") is not True:
                raise SummaryError(f"{label} long-prefill exact oracle failed")
            long_rates[role][suffix] = finite_float(
                long, "prefill_tokens_per_second", f"{label} long-prefill"
            )
            prompt_tokens = finite_float(long, "prompt_tokens", f"{label} long-prefill")
            wall_seconds = finite_float(long, "wall_seconds", f"{label} long-prefill")
            long_wall_rates[role][suffix] = prompt_tokens / wall_seconds
            mtp_rates[role][suffix] = finite_float(
                mtp, "decode_tokens_per_second", f"{label} MTP3 decode"
            )
            hashes["long"][label] = str(long.get("message_sha256", ""))
            hashes["mtp"][label] = str(mtp.get("message_sha256", ""))

    return {
        "candidate_variant": candidate_variant,
        "trial_count_per_variant": len(PAIR_SUFFIXES),
        "confidence_rule": {
            "interval": "two-sided 95% paired Student t, df=2",
            "credible_positive": "all pairs positive, interval lower bound positive, disjoint ranges",
            "credible_negative": "all pairs negative, interval upper bound negative, disjoint ranges",
        },
        "long_prefill_tokens_per_second": metric_summary(
            long_rates["baseline"], long_rates["candidate"]
        ),
        "long_wall_prompt_tokens_per_second": metric_summary(
            long_wall_rates["baseline"], long_wall_rates["candidate"]
        ),
        "mtp3_decode_tokens_per_second": metric_summary(
            mtp_rates["baseline"], mtp_rates["candidate"]
        ),
        "message_hashes": {
            **hashes,
            "long_all_equal": len(set(hashes["long"].values())) == 1,
            "mtp_all_equal": len(set(hashes["mtp"].values())) == 1,
        },
    }


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        summary = summarize(args.result_dir)
    except SummaryError as exc:
        print(f"summarize_profile_ab: {exc}", file=sys.stderr)
        return 1
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    compact = {
        "candidate_variant": summary["candidate_variant"],
        "long_prefill": summary["long_prefill_tokens_per_second"],
        "mtp3_decode": summary["mtp3_decode_tokens_per_second"],
    }
    print(json.dumps(compact, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
