#!/usr/bin/env python3
"""Reduce content-safe EXP-098 arms; reuse the registered paired redaction rule."""
from __future__ import annotations

import argparse
import json
from pathlib import Path

from run_exp097 import load_module
from run_exp098_native import save, sha_file, summarize


def paired_roles(mtp, dflash):
    a, b = ({r["id"]: r for r in arm["roles"]} for arm in (mtp, dflash))
    if set(a) != set(b) or len(a) != 84:
        raise ValueError("role arms must contain the same 84 cases")
    if any(a[k]["request_sha256"] != b[k]["request_sha256"] for k in a):
        raise ValueError("wire requests changed between arms")
    unequal = [k for k in a if a[k]["output_sha256"] != b[k]["output_sha256"]]
    sa, sb = summarize(mtp["roles"]), summarize(dflash["roles"])
    return {"request_pairs": len(a), "equal_output_pairs": len(a) - len(unequal),
            "unequal_case_ids": sorted(unequal),
            "equality_definition": "temperature-zero content, reasoning, tool names/arguments; transport IDs excluded",
            "mtp": sa, "dflash2": sb,
            "median_decode_ratio": sb["median_decode_tok_s"] / sa["median_decode_tok_s"],
            "weighted_decode_ratio": sb["weighted_decode_tok_s"] / sa["weighted_decode_tok_s"]}


def screen_rows(rows):
    return {r["id"]: {"output": r["output_sha256"], "leaks": r["leaks"],
                       "pass": r["pass"], "error": r["error"]} for r in rows}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--mtp", required=True, type=Path)
    p.add_argument("--dflash2", required=True, type=Path)
    p.add_argument("--redaction-screen", required=True, type=Path)
    p.add_argument("--out", required=True, type=Path)
    args = p.parse_args()
    mtp, dflash = (json.loads(path.read_text()) for path in (args.mtp, args.dflash2))
    for key in ("source_commit", "binary_sha256", "model_sha256", "model_bytes", "lane", "lane_shape", "corpus_sha256"):
        if mtp[key] != dflash[key]:
            raise ValueError("arm identity mismatch: " + key)
    if mtp["server"]["context"] != dflash["server"]["context"]:
        raise ValueError("unmatched contexts")
    if mtp["leak_screen"] != dflash["leak_screen"]:
        raise ValueError("unmatched leak screen")
    redaction = load_module("exp098_redaction", args.redaction_screen)
    # EXP-085 reused these two redaction margins with seed 20261001.
    redaction.PREREGISTRATION["seed"] = 20261001
    verdict = redaction.decide(screen_rows(mtp["redaction"]), screen_rows(dflash["redaction"]),
                              screen_rows(mtp["determinism"]), screen_rows(dflash["determinism"]))
    result = {"artifact_type": "ninfer_exp098_native_comparison", "schema_version": 1,
              "experiment": "EXP-098", "lane": mtp["lane"],
              "mtp_receipt_sha256": sha_file(args.mtp), "dflash2_receipt_sha256": sha_file(args.dflash2),
              "matched_context": mtp["server"]["context"], "role_comparison": paired_roles(mtp, dflash),
              "leak_comparison": verdict,
              "leak_method": {"scope": "EXP-085 leak subset, not a full quality verdict", "seed": 20261001,
                  "bootstrap_resamples": 20000, "determinism_subset_per_arm": 14,
                  "fresh_determinism_protocol": "EXP-063: v0/v1 of every control",
                  "secret_leak_ratio_upper_95_below": 1.10, "pass_rate_delta_lower_95_above": -0.05},
              "raw_prompts_outputs_or_secrets_included": False}
    save(args.out, result)


if __name__ == "__main__":
    main()
