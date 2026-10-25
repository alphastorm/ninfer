#!/usr/bin/env python3
"""EXP-097 content-safe role and context measurements against one isolated server.

Reuse the frozen qualification runner's request construction and the product
speculative probe's context construction. Neither private corpus nor output text
is persisted. Server logs must belong exclusively to this arm. This is performance
evidence, not a powered quality screen or a production-capacity acceptance.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import importlib.util
import json
import statistics
import sys
import time
from pathlib import Path


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def load_module(name, path):
    sys.path.insert(0, str(path.parent))
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def records(path):
    result = []
    for line in path.read_text().splitlines():
        try:
            result.append(json.loads(line))
        except json.JSONDecodeError:
            continue  # The logger may be appending its next line.
    return result


def bind_server(path, rows, start_ms):
    done = [r for r in records(path) if r.get("event") == "request_done"
            and r.get("timestamp_unix_ms", 0) >= start_ms]
    used = set()
    for row in rows:
        if row.get("http_status") != 200:
            continue
        matches = [i for i, r in enumerate(done) if i not in used
                   and r.get("result", {}).get("prompt_tokens") == row["prompt_tokens"]
                   and r.get("result", {}).get("completion_tokens") == row["completion_tokens"]
                   and r.get("timestamp_unix_ms", 0) >= row["submitted_unix_ms"]]
        if not matches:
            raise RuntimeError("missing request_done for " + row["id"])
        i = matches[0]
        used.add(i)
        event = done[i]
        timing = event.get("timings_seconds", {})
        spec = event.get("speculative", {})
        row["server"] = {key: timing.get(key) for key in ("prefill", "decode", "ttft", "total", "queue_wait")}
        row["server"].update({key: spec.get(key) for key in
                              ("rounds", "drafted_tokens", "accepted_tokens")})
        result = event.get("result", {})
        row["server"]["computed_prefill_tokens"] = result.get("computed_prefill_tokens")
        decode = timing.get("decode")
        rounds = spec.get("rounds")
        if not decode or not rounds:
            raise RuntimeError("missing decode/round metrics for " + row["id"])
        row["server"]["decode_tok_s"] = row["completion_tokens"] / decode
        row["server"]["ms_per_round"] = decode * 1000 / rounds
    if len(used) != sum(r.get("http_status") == 200 for r in rows):
        raise RuntimeError("non-bijective server matching")


def aggregate(rows):
    ok = [r for r in rows if r.get("http_status") == 200 and "server" in r]
    if not ok:
        return {"completed": 0, "attempted": len(rows)}
    tokens = sum(r["completion_tokens"] for r in ok)
    decode = sum(r["server"]["decode"] for r in ok)
    rounds = sum(r["server"]["rounds"] for r in ok)
    latency = sorted(r["server"]["decode"] * 1000 / r["completion_tokens"] for r in ok)
    # Nearest-rank empirical p95; no distributional confidence claim.
    p95 = latency[max(0, (95 * len(latency) + 99) // 100 - 1)]
    return {"attempted": len(rows), "completed": len(ok), "completion_tokens": tokens,
            "decode_s": decode, "rounds": rounds, "weighted_decode_tok_s": tokens / decode,
            "median_decode_tok_s": statistics.median(r["server"]["decode_tok_s"] for r in ok),
            "ms_per_round": decode * 1000 / rounds, "p95_decode_ms_per_token": p95}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--arm", required=True)
    p.add_argument("--base-url", required=True)
    p.add_argument("--api-key-file", type=Path, required=True)
    p.add_argument("--request-log", type=Path, required=True)
    p.add_argument("--qualification", type=Path, required=True)
    p.add_argument("--probe", type=Path, required=True)
    p.add_argument("--case-ids", type=Path, required=True)
    p.add_argument("--long-fixture", type=Path, required=True)
    p.add_argument("--identity", type=Path, required=True)
    p.add_argument("--receipt", type=Path, required=True)
    p.add_argument("--skip-roles", action="store_true")
    p.add_argument("--skip-contexts", action="store_true")
    args = p.parse_args()
    q = load_module("exp097_qualification", args.qualification / "scripts/run_qualification.py")
    probe = load_module("exp097_probe", args.probe)
    key = args.api_key_file.read_text().strip()
    selected = json.loads(args.case_ids.read_text())
    if len(selected) != 84 or len(set(selected)) != 84:
        raise ValueError("the EXP-095 common corpus must contain exactly 84 unique cases")
    cases, _ = q.load_cases([str(args.qualification / "cases")])
    by_id = {c["case_id"]: c for c in cases}
    if set(selected) - by_id.keys():
        raise ValueError("frozen cases unavailable")
    role_args = q.parse_args(["--model", "q38-ninfer", "--base-url", args.base_url + "/v1",
                             "--cases", str(args.qualification / "cases"), "--out", "unused",
                             "--concurrency", "1", "--reasoning-mode", "low",
                             "--reasoning-wire", "top-level", "--reasoning-token-headroom", "0",
                             "--timeout", "900", "--no-ssh"])
    role_args.api_key = key
    rows = []
    start_ms = int(time.time() * 1000)
    if not args.skip_roles:
        for case_id in selected:
            case = by_id[case_id]
            submitted = int(time.time() * 1000)
            warnings = []
            record = q.execute_case(case, role_args, str(args.qualification / "fixtures"),
                                    args.base_url + "/v1/chat/completions", not rows, warnings, None)
            if warnings:
                raise RuntimeError("role prompt source resolution changed for " + case_id)
            usage = record["usage"]
            tools = [{k: v for k, v in t.items() if k != "id"} for t in record["tool_calls"]]
            row = {"id": case_id, "workload": case["class"], "kind": "role",
                   "submitted_unix_ms": submitted, "case_sha256": record["case_sha256"],
                   "request_sha256": record["request_sha256"], "attempts": record["attempt"],
                   "http_status": 200 if usage.get("completion_tokens") is not None else None,
                   "prompt_tokens": usage.get("prompt_tokens"),
                   "completion_tokens": usage.get("completion_tokens"),
                   "client_verdict": (record["error"] or "answered").split(":", 1)[0],
                   "wall_ms": record["wall_ms"], "ttft_ms": record["ttft_ms"],
                   "output_sha256": digest([record["content"], record["reasoning"], tools])}
            rows.append(row)
            print(case_id, row["client_verdict"], flush=True)
    document = probe.document_text(args.long_fixture)
    ratio = len(document) / probe.FIXTURE_PROMPT_TOKENS

    def context_request(context, index, pair):
        # Stable per-workload input across arms, distinct single/pair roots.
        nonce = f"exp097-{'pair' if pair else 'single'}-{context}-{index}"
        messages = [{"role": "user", "content": probe.prompt(document, ratio, context, nonce)}]
        result = probe.chat(args.base_url, key, "q38-ninfer", messages, 1024 if pair else 2048, 1800)
        # Never persist raw outputs, remote exception messages, or HTTP bodies.
        row = {k: result.get(k) for k in ("http_status", "wall_s", "prompt_tokens", "completion_tokens",
                                         "cached_tokens", "finish_reason", "output_sha256")}
        row.update(id=nonce, kind="pair" if pair else "single", context=context,
                   request_sha256=digest(messages), submitted_unix_ms=int(result["submitted_unix_s"] * 1000),
                   completed_unix_s=result.get("completed_unix_s"))
        return row

    pairs = []
    if not args.skip_contexts:
        for context in (0, 32000, 64000, 120000):
            rows.append(context_request(context, 0, False))
            with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
                pair = list(pool.map(lambda i: context_request(context, i, True), (1, 2)))
            rows.extend(pair)
            if all(r["http_status"] == 200 for r in pair):
                span = max(r["completed_unix_s"] for r in pair) - min(r["submitted_unix_ms"] for r in pair) / 1000
                pairs.append({"context": context, "wall_span_s": span,
                              "client_aggregate_tok_s": sum(r["completion_tokens"] for r in pair) / span})
            print("context", context, [r["http_status"] for r in pair], flush=True)
    time.sleep(2)  # Allow asynchronous final request-log publication.
    bind_server(args.request_log, rows, start_ms)
    starts = [r for r in records(args.request_log) if r.get("event") == "server_start"]
    identity = json.loads(args.identity.read_text())
    receipt = {"artifact_type": "omp_ninfer_exp097_arm", "schema_version": 1,
               "experiment": "EXP-097", "arm": args.arm, "identity": identity,
               "runner_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
               "qualification_runner_sha256": hashlib.sha256((args.qualification / "scripts/run_qualification.py").read_bytes()).hexdigest(),
               "probe_sha256": hashlib.sha256(args.probe.read_bytes()).hexdigest(),
               "case_ids_sha256": digest(selected), "long_fixture_sha256": hashlib.sha256(args.long_fixture.read_bytes()).hexdigest(),
               "shape": {"spec": "dflash2", "draft_tokens": 7, "kv_dtype": "bf16", "max_context": 131072,
                         "kv_capacity": 131072, "prefill_chunk": 1024, "max_concurrency": 2},
               "server_start": probe.startup(starts, start_ms), "requests": rows, "pairs": pairs,
               "role_summary": aggregate([r for r in rows if r["kind"] == "role"]),
               "workloads": {w: aggregate([r for r in rows if r.get("workload") == w])
                             for w in sorted({r["workload"] for r in rows if r["kind"] == "role"})},
               "raw_prompts_outputs_or_secrets_included": False}
    encoded = json.dumps(receipt, indent=2) + "\n"
    if any(marker in encoded for marker in ("/home/", "/Users/", "C:\\Users\\", "nyc-pc", "sf-old", "sf-pc")):
        raise ValueError("receipt contains a private marker")
    args.receipt.parent.mkdir(parents=True, exist_ok=True)
    args.receipt.write_text(encoded)
    print(json.dumps(receipt["role_summary"]), flush=True)
    return int(any(r["http_status"] != 200 for r in rows))


if __name__ == "__main__":
    raise SystemExit(main())
