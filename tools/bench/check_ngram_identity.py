#!/usr/bin/env python3
"""Capture exact greedy output projections and token IDs on isolated fresh servers.

Compare base, candidate-omitted, candidate-zero; optionally add candidate-enabled
zero-copy controls. This does not start a server or grant permission to use a GPU.
Private response bytes stay in the supplied evidence directory, not in the report.
"""
import argparse
import hashlib
import json
from pathlib import Path
import time
import urllib.request

CASES = {
    "ready": "Answer with exactly READY and nothing else.",
    "arithmetic": "What is 823 plus 419? Answer with the integer only.",
    "small-explanation": "In two short sentences, explain why a database uses a write-ahead log.",
    "read": "A tool returned a Python file containing a function named lookup. Confirm receipt with exactly READ.",
}
COPY_FIELDS = ("ngram_rounds", "ngram_drafted_tokens", "ngram_accepted_tokens")


def capture(args):
    args.out.mkdir(parents=True, exist_ok=False)
    key = args.key.read_text().strip()
    start = int(time.time() * 1000)
    rows = []
    for name, prompt in CASES.items():
        payload = {"model": "q38-ninfer", "messages": [{"role": "user", "content": prompt}],
                   "temperature": 0, "reasoning_effort": "none", "max_completion_tokens": 128}
        request = urllib.request.Request(args.endpoint.rstrip("/") + "/v1/chat/completions",
            data=json.dumps(payload).encode(), headers={"Content-Type": "application/json",
                                                       "Authorization": "Bearer " + key})
        with urllib.request.urlopen(request, timeout=180) as response:
            body = json.load(response)
        choice = body["choices"][0]
        message = choice["message"]
        projection = {"content": message.get("content"), "reasoning": message.get("reasoning_content"),
                      "tool_calls": [{k: v for k, v in call.items() if k != "id"}
                                     for call in message.get("tool_calls", [])],
                      "finish_reason": choice["finish_reason"]}
        encoded = json.dumps(projection, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode()
        (args.out / (name + ".bytes")).write_bytes(encoded)
        rows.append({"id": name, "usage": body["usage"],
                     "output_sha256": hashlib.sha256(encoded).hexdigest()})
    time.sleep(2)  # Bounded log-publication grace, never an inference retry.
    events = [json.loads(line) for line in args.log.read_text().splitlines() if line.strip()]
    events = [event for event in events if event.get("event") == "request_done"
              and event.get("timestamp_unix_ms", 0) >= start]
    if len(events) != len(rows):
        raise RuntimeError("probe requires an isolated server/request log")
    for row, event in zip(rows, events):
        for name in ("prompt_tokens", "completion_tokens"):
            if event["result"][name] != row["usage"][name]:
                raise RuntimeError("request-log association mismatch")
        spec = event["speculative"]
        if args.enabled and not all(name in spec for name in COPY_FIELDS):
            raise RuntimeError("enabled copy telemetry is missing")
        if any(spec.get(name, 0) != 0 for name in COPY_FIELDS):
            raise RuntimeError("zero-copy probe exercised copying: " + row["id"])
        tokens = event["result"]["generated_token_ids"]
        if len(tokens) != row["usage"]["completion_tokens"]:
            raise RuntimeError("generated token evidence is incomplete")
        (args.out / (row["id"] + ".tokens")).write_text(json.dumps(tokens, separators=(",", ":")))
        row["copy_rounds"] = 0
        row["decode_seconds"] = event["timings_seconds"]["decode"]
    receipt = {"arm": args.arm, "source_commit": args.source_commit,
               "binary_sha256": hashlib.sha256(args.binary.read_bytes()).hexdigest(),
               "copy_enabled": args.enabled, "requests": rows}
    (args.out / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")


def compare(args):
    if len(args.arms) < 3 or len(set(args.arms)) != len(args.arms):
        raise ValueError("at least three distinct base/omitted/zero arms are required")
    receipts = [json.loads((args.root / arm / "receipt.json").read_text()) for arm in args.arms]
    for arm, receipt in zip(args.arms, receipts):
        if receipt["arm"] != arm or [r["id"] for r in receipt["requests"]] != list(CASES):
            raise ValueError("receipt arm or corpus mismatch")
    rows = []
    for name in CASES:
        outputs = [(args.root / arm / (name + ".bytes")).read_bytes() for arm in args.arms]
        tokens = [(args.root / arm / (name + ".tokens")).read_bytes() for arm in args.arms]
        copied = [next(r for r in receipt["requests"] if r["id"] == name)["copy_rounds"]
                  for receipt in receipts]
        rows.append({"id": name, "actual_bytes_equal": all(x == outputs[0] for x in outputs),
                     "token_ids_equal": all(x == tokens[0] for x in tokens), "copy_rounds": copied})
    passed = all(row["actual_bytes_equal"] and row["token_ids_equal"] and not any(row["copy_rounds"])
                 for row in rows)
    args.out.write_text(json.dumps({"passed": passed, "arms": args.arms, "requests": rows,
                                    "raw_content_included": False}, indent=2) + "\n")
    if not passed:
        raise SystemExit(1)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    subs = parser.add_subparsers(dest="command", required=True)
    cap = subs.add_parser("capture")
    cap.add_argument("--arm", required=True)
    cap.add_argument("--endpoint", required=True)
    cap.add_argument("--source-commit", required=True)
    for flag in ("key", "log", "binary", "out"):
        cap.add_argument("--" + flag, type=Path, required=True)
    cap.add_argument("--enabled", action="store_true")
    comp = subs.add_parser("compare")
    comp.add_argument("--root", type=Path, required=True)
    comp.add_argument("--out", type=Path, required=True)
    comp.add_argument("--arms", nargs="+", default=["base", "omitted", "zero"])
    args = parser.parse_args()
    (capture if args.command == "capture" else compare)(args)
