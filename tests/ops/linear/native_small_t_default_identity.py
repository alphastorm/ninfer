#!/usr/bin/env python3
"""Compare OFF translation units with a base revision using the configured compilers.

Only source-location diagnostics (__FILE__/__LINE__), preprocessor line markers and
empty lines are excluded. No arithmetic tokens, dispatch code or flags are normalized.
Run against an OFF compile database; the receipt records every compared source hash.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import tarfile


SOURCES = (
    "src/ops/linear/add/q5/shapes/n5120_k6144.cu",
    "src/ops/linear/add/q5/shapes/n5120_k17408.cu",
    "src/ops/linear/split/q4_q5/shapes/n7168_n7168_k5120/shape.cpp",
    "src/ops/linear/split/q4_q5/shapes/n7168_n7168_k5120/small.cu",
    "src/ops/linear/swiglu/q4/shapes/n34816_k5120/small.cu",
    "src/ops/linear_attention/gated_delta_net/input_conv/q4_q5/projection.cpp",
    "src/ops/linear_attention/gated_delta_net/input_conv/q4_q5/projection_independent.cu",
)


def preprocess_command(entry, root, source_root):
    args = entry.get("arguments") or shlex.split(entry["command"])
    if any("NINFER_NATIVE_SMALL_T_TC" in arg or "NINFER_NATIVE_PREFILL_GROUP64_A8" in arg
           for arg in args):
        raise ValueError("identity proof requires both numerical options OFF")
    result = []
    skip = False
    for arg in args:
        if skip:
            skip = False
            continue
        if arg in ("-o", "-MF", "-MT", "-MQ"):
            skip = True
            continue
        if arg in ("-c", "-MD", "-MMD"):
            continue
        # Keep generated build headers at the configured path; source headers
        # and the translation unit must come from the selected source revision.
        for subtree in ("src", "include", "third_party"):
            arg = arg.replace(str(root / subtree), str(source_root / subtree))
        result.append(arg)
    return result + ["-E", '-D__FILE__="source"', "-D__LINE__=0"]


def normalized(text):
    return "\n".join(line.rstrip() for line in text.splitlines()
                     if line.strip() and not re.match(r"^\s*#(?:\s|line\b)", line)) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", required=True, type=Path)
    parser.add_argument("--base", required=True)
    parser.add_argument("--work", required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=False)
    base = subprocess.check_output(["git", "rev-parse", args.base], cwd=root, text=True).strip()
    archive = work / "base.tar"
    subprocess.run(["git", "archive", "--format=tar", f"--output={archive}", base],
                   cwd=root, check=True)
    baseline = work / "base"
    baseline.mkdir()
    with tarfile.open(archive) as bundle:
        bundle.extractall(baseline, filter="data")
    entries = json.loads((args.build / "compile_commands.json").read_text(encoding="utf-8"))
    by_file = {Path(entry["file"]).resolve(): entry for entry in entries}
    report = {"base": base, "head": subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
        "normalization": "fixed diagnostic __FILE__/__LINE__; remove line markers and empty lines",
        "sources": []}
    for relative in SOURCES:
        entry = by_file[root / relative]
        outputs = []
        for label, source_root in (("base", baseline), ("head", root)):
            command = preprocess_command(entry, root, source_root)
            result = subprocess.run(command, cwd=entry["directory"], check=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            text = normalized(result.stdout)
            stem = relative.replace("/", "_")
            (work / f"{stem}.{label}.i").write_text(text, encoding="utf-8")
            (work / f"{stem}.{label}.stderr").write_text(result.stderr, encoding="utf-8")
            outputs.append(text)
        equal = outputs[0] == outputs[1]
        report["sources"].append({"source": relative, "equal": equal,
            "base_sha256": hashlib.sha256(outputs[0].encode()).hexdigest(),
            "head_sha256": hashlib.sha256(outputs[1].encode()).hexdigest()})
        print(f"{'PASS' if equal else 'FAIL'} OFF preprocessing: {relative}", flush=True)
    (work / "receipt.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2), flush=True)
    if not all(row["equal"] for row in report["sources"]):
        raise SystemExit("OFF preprocessing differs; inspect retained .i files")


if __name__ == "__main__":
    main()
