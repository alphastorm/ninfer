#!/usr/bin/env python3
"""Validate BUILD T evidence and replay its integer decisions, not counterfactual frontiers."""
import argparse
import json
from pathlib import Path
import subprocess


def replay(trace):
    if "speculative" in trace:
        log = trace
        trace = log["speculative"]["dflash_trace"]
        if (trace["request_sha256"] != log["request"]["client_identity"]["request_sha256"] or
                trace["output_token_ids"] != log["result"]["generated_token_ids"] or
                len(trace["prompt_token_ids"]) != log["result"]["prompt_tokens"]):
            raise ValueError("trace does not bind the request/result")
    if trace["schema_version"] != 1:
        raise ValueError("unsupported DFlash trace schema")
    sha = trace["request_sha256"]
    if len(sha) != 64 or any(c not in "0123456789abcdef" for c in sha):
        raise ValueError("missing canonical request SHA-256")
    rows = trace["rounds"]
    prompt, output = trace["prompt_token_ids"], trace["output_token_ids"]
    if len(rows) > 131072 or len(prompt) + len(output) > 262144:
        raise ValueError("incomplete evidence: trace cap exceeded")
    if any(type(t) is not int or t < 0 for t in prompt + output):
        raise ValueError("invalid exact token IDs")
    policy = trace["policy"]
    mode, r, h, cap = (policy[k] for k in ("mode", "rounds", "threshold", "cap"))
    if (mode not in range(4) or r not in (8, 16, 32) or
            h not in {r * q // 4 for q in range(10, 14)} or cap not in (1, 2, 3)):
        raise ValueError("policy outside frozen grid")
    if mode == 3 and (r, h, cap) != (16, 44, 3):
        raise ValueError("CAL policy changed")
    history, armed, capped, lows, decisions = [], mode == 2, mode == 2, [], []
    previous_end = 0
    for ordinal, row in enumerate(rows):
        k, p, a, licensed, committed = (row[name] for name in (
            "physical_k", "extent", "accepted", "licensed", "committed"))
        for name in ("previous_append", "batch", "row", "lane", "graph_class",
                     "graph_envelope", "frontier", "output_offset"):
            if type(row[name]) is not int or row[name] < 0:
                raise ValueError(f"invalid {name}")
        for name in ("budget_clipped", "capacity_clipped", "terminal", "discarded",
                     "armed", "capped", "graph_replay"):
            if type(row[name]) is not bool:
                raise ValueError(f"invalid {name}")
        if (row["ordinal"] != ordinal or not 1 <= k <= 15 or
                not 0 <= a <= p <= k or licensed != a + 1 or
                not 0 <= committed <= licensed or
                not 1 <= row["batch"] <= 8 or not 0 <= row["row"] < row["batch"] or
                not 0 <= row["lane"] < 8 or row["output_offset"] < previous_end or
                row["frontier"] != len(prompt) + row["output_offset"] - 1 or
                row["output_offset"] + committed > len(output) or
                (row["discarded"] and committed != 0) or
                (committed != licensed and not row["terminal"])):
            raise ValueError(f"invalid commit-bound record {ordinal}")
        previous_end = row["output_offset"] + committed
        if row["terminal"] and ordinal != len(rows) - 1:
            raise ValueError("trace continues after terminal commit")
        expected_cap = mode == 2 or (armed and capped)
        if (row["armed"], row["capped"]) != (armed, expected_cap):
            raise ValueError(f"C++/replay decision mismatch at round {ordinal}")
        limit = min(k, cap) if expected_cap else k
        if p > limit or (not (row["budget_clipped"] or row["capacity_clipped"]) and p != limit):
            raise ValueError("extent does not match decision")
        if mode != 0 and k != 7:
            raise ValueError("governor changed physical width")
        decisions.append({"armed": armed, "capped": expected_cap})
        informative = (not row["discarded"] and not row["budget_clipped"] and
                       not row["capacity_clipped"] and p > 0 and committed == licensed)
        lows.append(a <= cap if informative and p == 7 else None)
        if not informative or mode in (0, 2):
            continue
        if not armed:
            if p != 7:
                continue
            history = (history + [a])[-r:]
            armed = len(history) == r and (mode == 3 or sum(history) <= h)
            if armed:
                capped = mode == 3 or a <= cap
        elif mode == 3:
            capped = not capped
        else:
            capped = a <= cap if p == 7 else a < p
    rates = {}
    for lag in range(1, 9):
        pairs = [(lows[i], lows[i + lag]) for i in range(len(lows) - lag)
                 if lows[i] is not None and lows[i + lag] is not None]
        previous_low = sum(first for first, _ in pairs)
        rates[str(lag)] = {
            "previous_low": previous_low,
            "both_low": sum(first and second for first, second in pairs),
        }
    runs, run = [], 0
    for low in lows + [False]:
        if low:
            run += 1
        elif run:
            runs.append(run)
            run = 0
    return {"request_sha256": sha, "rounds": len(rows), "decisions": decisions,
            "informative_long_rounds": sum(v is not None for v in lows),
            "low_rounds": sum(v is True for v in lows),
            "low_runs": runs, "lag_counts": rates}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", nargs="?", type=Path, help="CLI trace JSON or serve request JSONL")
    parser.add_argument("--cpp-fixture", type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.cpp_fixture:
        trace = json.loads(subprocess.check_output([str(args.cpp_fixture), "--fixture"], text=True))
        result = replay(trace)
        assert result["decisions"][7] == {"armed": False, "capped": False}
        assert result["decisions"][8] == {"armed": True, "capped": True}
        for mutation in ("schema", "overflow", "partial", "decision"):
            bad = json.loads(json.dumps(trace))
            if mutation == "schema":
                bad["schema_version"] = 2
            elif mutation == "overflow":
                bad["output_token_ids"] = [1] * 262145
            elif mutation == "partial":
                bad["rounds"][0]["committed"] = 0
            else:
                bad["rounds"][8]["armed"] = False
            try:
                replay(bad)
            except ValueError:
                continue
            raise AssertionError(f"incomplete evidence accepted: {mutation}")
        print("OK C++ trace / independent Python integer replay")
    elif args.input:
        found = False
        for line in args.input.read_text(encoding="utf-8").splitlines():
            value = json.loads(line)
            if "schema_version" in value and "policy" in value:
                print(json.dumps(replay(value)))
                found = True
            elif value.get("speculative", {}).get("dflash_trace") is not None:
                print(json.dumps(replay(value)))
                found = True
            elif value.get("event") in ("request_done", "request_failed", "request_rejected"):
                raise ValueError("incomplete evidence: request has no trace")
        if not found:
            raise ValueError("no DFlash trace evidence")
    else:
        parser.error("input is required")


if __name__ == "__main__":
    main()
