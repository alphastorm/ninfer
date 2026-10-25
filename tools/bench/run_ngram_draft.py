#!/usr/bin/env python3
"""S1 matched-build ABBA arm runner; retain only identities, counts and output hashes.

The frozen role subset is an off-policy control. The separate synthetic tool-result
Read -> Edit -> explain chains exercise copying; they are not mislabeled private
role-corpus cases. Context prompts reuse the EXP-095 single-request depth probe.
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
import urllib.request
from pathlib import Path

ROLE_IDS = ("log-001", "log-002", "repo-001", "repo-005", "struct-002", "struct-003")
DEPTHS = (0, 32000, 64000, 120000)
PRIVATE_MARKERS = ("/home/", "/Users/", "C:\\Users\\", "nyc-pc", "sf-pc", "sf-old")


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def module(name, path):
    sys.path.insert(0, str(path.parent))
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def write_receipt(path, value):
    encoded = json.dumps(value, indent=2) + "\n"
    if any(marker in encoded for marker in PRIVATE_MARKERS):
        raise ValueError("receipt contains a private marker")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(encoded)


def records(path):
    result = []
    for line in path.read_text().splitlines():
        try:
            result.append(json.loads(line))
        except json.JSONDecodeError:
            continue  # The server may still be appending its final event.
    return result


def fixture(index):
    lines = ['"""Deterministic named weight lookup."""', "", "DEFAULT_LIMIT = 64", "", "WEIGHTS = {"]
    for item in range(72 + index * 3):
        lines.append(f'    "item_{index}_{item:03d}": {item * 17 + index},')
    lines.extend(["}", "", "", "def lookup(names, limit=DEFAULT_LIMIT):",
                  '    """Return known names in input order, without exceeding the limit."""',
                  "    if limit < 0:", '        raise ValueError("negative limit")',
                  "    result = []", "    for name in names:", "        if len(result) >= limit:",
                  "            break", "        if name in WEIGHTS:",
                  "            result.append((name, WEIGHTS[name]))", "    return result", ""])
    return "\n".join(lines)


def bind_log(path, rows, started, require_copy_counters=True):
    done = [r for r in records(path) if r.get("event") == "request_done"
            and r.get("timestamp_unix_ms", 0) >= started]
    used = set()
    for row in rows:
        if row["http_status"] != 200:
            continue
        candidates = [i for i, event in enumerate(done) if i not in used
                      and event.get("result", {}).get("prompt_tokens") == row["prompt_tokens"]
                      and event.get("result", {}).get("completion_tokens") == row["completion_tokens"]
                      and event.get("timestamp_unix_ms", 0) >= row["submitted_unix_ms"]]
        if not candidates:
            raise RuntimeError("missing request log for " + row["id"])
        i = candidates[0]
        used.add(i)
        event = done[i]
        timing = event.get("timings_seconds", {})
        speculative = event.get("speculative", {})
        row["server"] = {key: timing.get(key) for key in ("decode", "prefill", "ttft")}
        row["server"].update({key: speculative.get(key) for key in
                              ("rounds", "drafted_tokens", "accepted_tokens", "ngram_rounds",
                               "ngram_drafted_tokens", "ngram_accepted_tokens")})
        if row["kind"] != "read" and not row["server"]["decode"]:
            raise RuntimeError("missing decode timing for " + row["id"])
        if require_copy_counters and any(row["server"][key] is None for key in
               ("ngram_rounds", "ngram_drafted_tokens", "ngram_accepted_tokens")):
            raise RuntimeError("copy counters unavailable")


def run(args):
    probe = module("s1_probe", args.probe)
    qualification = module("s1_qualification", args.qualification / "scripts/run_qualification.py")
    key = args.api_key_file.read_text().strip()
    rows = []
    started = int(time.time() * 1000)

    def chat(case_id, kind, messages, budget, response_format=None):
        if response_format is None:
            result = probe.chat(args.base_url, key, "q38-ninfer", messages, budget, 1800)
        else:
            request = urllib.request.Request(
                args.base_url + "/v1/chat/completions",
                data=json.dumps({"model": "q38-ninfer", "messages": messages,
                                 "max_completion_tokens": budget, "temperature": 0,
                                 "reasoning_effort": "none",
                                 "response_format": response_format}).encode(),
                headers={"Authorization": "Bearer " + key, "Content-Type": "application/json"})
            submitted = time.time()
            with urllib.request.urlopen(request, timeout=1800) as response:
                body = json.load(response)
            text = body["choices"][0]["message"]["content"]
            result = {"http_status": 200, "submitted_unix_s": submitted, "text": text,
                      "output_sha256": hashlib.sha256(text.encode()).hexdigest(),
                      "finish_reason": body["choices"][0]["finish_reason"],
                      "prompt_tokens": body["usage"]["prompt_tokens"],
                      "completion_tokens": body["usage"]["completion_tokens"]}
        row = {key: result.get(key) for key in
               ("http_status", "prompt_tokens", "completion_tokens", "finish_reason", "output_sha256")}
        row.update(id=case_id, kind=kind, request_sha256=digest([messages, response_format]),
                   submitted_unix_ms=int(result["submitted_unix_s"] * 1000))
        rows.append(row)
        if result.get("http_status") != 200:
            raise RuntimeError("HTTP failure for " + case_id)
        return result["text"]

    for index in range(3):
        source = fixture(index)
        messages = [
            {"role": "system", "content": "You edit code accurately. Follow the requested output format exactly."},
            {"role": "user", "content": "Read lookup.py using Read. Do not change it yet."},
            {"role": "assistant", "content": None, "tool_calls": [
                {"id": "call_read", "type": "function", "function":
                 {"name": "Read", "arguments": '{"path":"lookup.py"}'}}]},
            {"role": "tool", "tool_call_id": "call_read", "content": source},
            {"role": "user", "content": "Confirm that you read the file. Answer only READY."},
        ]
        text = chat(f"chain-{index}-read", "read", messages, 16)
        messages.extend([
            {"role": "assistant", "content": text},
            {"role": "user", "content": "Change DEFAULT_LIMIT from 64 to 96. Output the entire revised file verbatim with only that change, without markdown fences or explanation."},
        ])
        text = chat(f"chain-{index}-edit", "edit", messages, 4096)
        rows[-1]["exact_edit"] = text.strip() == source.replace("DEFAULT_LIMIT = 64", "DEFAULT_LIMIT = 96").strip()
        messages.extend([
            {"role": "assistant", "content": text},
            {"role": "user", "content": "Explain the behavioral effect of the change in 150 words. Do not repeat the code."},
        ])
        chat(f"chain-{index}-explain", "explain", messages, 384)

    paired_copy = [
        {"role": "user", "content": "Read this Python file:\n\n" + fixture(4)},
        {"role": "assistant", "content": "READY"},
        {"role": "user", "content": "Change DEFAULT_LIMIT from 64 to 96. Output the entire revised file verbatim with only that change, without markdown fences or explanation."},
    ]
    paired_control = [{"role": "user", "content":
                       "Explain how a relational database executes a recursive common table expression. "
                       "Cover termination, cycle detection, indexes, and three practical examples in detail."}]
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        jobs = [pool.submit(chat, "paired-copy", "paired_copy", paired_copy, 1024),
                pool.submit(chat, "paired-control", "paired_control", paired_control, 512)]
        for job in jobs:
            job.result()

    copied_payload = " ".join(f"entry{item:03d}" for item in range(100))
    json_source = json.dumps({"payload": copied_payload, "revision": 1}, separators=(",", ":"))
    constrained = [{"role": "user", "content":
                    "Return this exact JSON object with only revision changed to 2. "
                    "Preserve the entire payload string exactly:\n" + json_source}]
    response_format = {"type": "json_schema", "json_schema": {
        "name": "copy_record", "strict": True, "schema": {
            "type": "object", "properties": {"payload": {"type": "string"},
                                              "revision": {"type": "integer"}},
            "required": ["payload", "revision"], "additionalProperties": False}}}
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        copied = pool.submit(chat, "constrained-copy", "constraint_copy", constrained, 1024,
                             response_format)
        control = pool.submit(chat, "constrained-partner", "constraint_control",
                              paired_control, 384)
        constrained_text = copied.result()
        control.result()
    constrained_exact = json.loads(constrained_text) == {"payload": copied_payload, "revision": 2}

    cases, _ = qualification.load_cases([str(args.qualification / "cases")])
    by_id = {case["case_id"]: case for case in cases}
    role_args = qualification.parse_args([
        "--model", "q38-ninfer", "--base-url", args.base_url + "/v1",
        "--cases", str(args.qualification / "cases"), "--out", "unused",
        "--concurrency", "1", "--reasoning-mode", "low", "--reasoning-wire", "top-level",
        "--reasoning-token-headroom", "0", "--timeout", "900", "--no-ssh"])
    role_args.api_key = key
    for case_id in ROLE_IDS:
        warnings = []
        before = int(time.time() * 1000)
        result = qualification.execute_case(by_id[case_id], role_args,
                    str(args.qualification / "fixtures"), args.base_url + "/v1/chat/completions",
                    False, warnings, None)
        if warnings or result["attempt"] != 1:
            raise RuntimeError("role request construction or first attempt changed")
        usage = result["usage"]
        tools = [{k: v for k, v in item.items() if k != "id"} for item in result["tool_calls"]]
        rows.append({"id": case_id, "kind": "role", "submitted_unix_ms": before,
                     "request_sha256": result["request_sha256"],
                     "output_sha256": digest([result["content"], result["reasoning"], tools]),
                     "http_status": 200 if usage.get("completion_tokens") is not None else None,
                     "prompt_tokens": usage.get("prompt_tokens"),
                     "completion_tokens": usage.get("completion_tokens")})

    document = probe.document_text(args.long_fixture)
    ratio = len(document) / probe.FIXTURE_PROMPT_TOKENS
    for depth in DEPTHS:
        messages = [{"role": "user", "content": probe.prompt(document, ratio, depth, f"s1-depth-{depth}")}]
        chat(f"depth-{depth}", "depth", messages, 2048)

    # Bounded publication grace, not a repeat of any inference request.
    time.sleep(2)
    bind_log(args.request_log, rows, started, require_copy_counters=not args.reference)
    receipt = {"schema_version": 1, "experiment": "S1", "arm": args.arm,
               "reference": args.reference,
               "copy_window": args.copy_window, "source_commit": args.source_commit,
               "binary_sha256": hashlib.sha256(args.binary.read_bytes()).hexdigest(),
               "artifact_sha256": args.artifact_sha256,
               "runner_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
               "probe_sha256": hashlib.sha256(args.probe.read_bytes()).hexdigest(),
               "qualification_sha256": hashlib.sha256((args.qualification / "scripts/run_qualification.py").read_bytes()).hexdigest(),
               "long_fixture_sha256": hashlib.sha256(args.long_fixture.read_bytes()).hexdigest(),
               "shape": {"spec": "dflash2", "draft_tokens": 7, "max_concurrency": 2,
                         "max_context": 131072, "kv_dtype": "bf16", "prefill_chunk": 1024},
               "read_edit_explain_source": "synthetic tool-result fixture, separate from frozen role controls",
               "mixed_constraint_exact": constrained_exact,
               "requests": rows, "raw_content_included": False}
    write_receipt(args.receipt, receipt)
    print(json.dumps({"arm": args.arm, "requests": len(rows)}), flush=True)
    return 0


def compare(args):
    arms = [json.loads(path.read_text()) for path in args.receipts]
    if len(arms) != 4 or [a["copy_window"] for a in arms] != [0, 15, 15, 0]:
        raise ValueError("expected matched off/on/on/off arms")
    baseline = json.loads(args.baseline.read_text())
    if not baseline["reference"] or baseline["copy_window"] != 0 or any(a["reference"] for a in arms):
        raise ValueError("expected a separate unmodified-base greedy reference")
    for field in ("binary_sha256", "artifact_sha256", "source_commit", "runner_sha256",
                  "probe_sha256", "qualification_sha256", "long_fixture_sha256", "shape"):
        if any(a[field] != arms[0][field] for a in arms):
            raise ValueError("arm identity mismatch: " + field)
        if field not in ("binary_sha256", "source_commit") and baseline[field] != arms[0][field]:
            raise ValueError("baseline fixture mismatch: " + field)
    indexes = [{r["id"]: r for r in a["requests"]} for a in arms]
    reference = {r["id"]: r for r in baseline["requests"]}
    if any(set(index) != set(indexes[0]) for index in indexes) or set(reference) != set(indexes[0]):
        raise ValueError("arm request set mismatch")
    rows = []
    for case_id in indexes[0]:
        requests = [index[case_id] for index in indexes]
        same_input = len({r["request_sha256"] for r in requests} |
                         {reference[case_id]["request_sha256"]}) == 1
        same_output = len({r["output_sha256"] for r in requests}) == 1
        off_identical = requests[0]["output_sha256"] == requests[3]["output_sha256"]
        baseline_equal = off_identical and requests[0]["output_sha256"] == reference[case_id]["output_sha256"]
        rates = [r["completion_tokens"] / r["server"]["decode"] if r["server"]["decode"] else None
                 for r in requests]
        change = None if None in rates else 100 * (statistics.median(rates[1:3]) /
                                                   statistics.median([rates[0], rates[3]]) - 1)
        rows.append({"id": case_id, "kind": requests[0]["kind"], "same_input": same_input,
                     "greedy_equal": same_output, "disabled_equal": off_identical,
                     "baseline_disabled_equal": baseline_equal,
                     "decode_tok_s_abba": rates, "decode_change_percent": change,
                     "copy_proposed_abba": [r["server"]["ngram_drafted_tokens"] for r in requests],
                     "copy_accepted_abba": [r["server"]["ngram_accepted_tokens"] for r in requests]})
    groups = {}
    for kind in ("read", "edit", "explain", "role", "depth", "paired_copy", "paired_control",
                 "constraint_copy", "constraint_control"):
        rates = []
        for arm in arms:
            selected = [r for r in arm["requests"] if r["kind"] == kind]
            seconds = sum(r["server"]["decode"] for r in selected)
            rates.append(sum(r["completion_tokens"] for r in selected) / seconds if seconds else None)
        groups[kind] = {"weighted_decode_tok_s_abba": rates,
                        "change_percent": None if None in rates else
                            100 * (statistics.median(rates[1:3]) /
                                   statistics.median([rates[0], rates[3]]) - 1)}
    input_equal = all(r["same_input"] for r in rows)
    output_equal = all(r["greedy_equal"] and r["baseline_disabled_equal"] for r in rows)
    edit_exact = all(r.get("exact_edit", True) for a in arms for r in a["requests"])
    constraint_exact = all(a["mixed_constraint_exact"] for a in arms)
    copy_exercised = any(r["copy_accepted_abba"][1] > 0 for r in rows if r["kind"] == "edit")
    controls = [groups[k]["change_percent"] for k in
                ("explain", "role", "depth", "paired_control", "constraint_control")]
    if groups["read"]["change_percent"] is not None:
        controls.append(groups["read"]["change_percent"])
    controls.extend(r["decode_change_percent"] for r in rows if r["kind"] == "depth")
    passed = input_equal and output_equal and edit_exact and constraint_exact and copy_exercised and \
        groups["edit"]["change_percent"] >= 15 and min(controls) >= -0.5
    write_receipt(args.receipt, {"schema_version": 1, "experiment": "S1", "recommendation": "go" if passed else "no-go",
        "binary_sha256": arms[0]["binary_sha256"], "source_commit": arms[0]["source_commit"],
        "baseline_source_commit": baseline["source_commit"],
        "baseline_binary_sha256": baseline["binary_sha256"],
        "baseline_receipt": args.baseline.name,
        "arm_receipts": [p.name for p in args.receipts], "groups": groups, "requests": rows,
        "acceptance": {"edit_gain_min_percent": 15, "other_loss_max_percent": 0.5,
                       "matched_inputs": input_equal, "greedy_equal": output_equal,
                       "baseline_disabled_equal": all(r["baseline_disabled_equal"] for r in rows),
                       "edits_exact": edit_exact, "accepted_copies_exercised": copy_exercised,
                       "mixed_constraint_exact": constraint_exact,
                       "read_decode_measurable": groups["read"]["change_percent"] is not None,
                       "passed": passed},
        "scope": "One bounded ABBA experiment, not a powered statistical or release qualification.",
        "raw_content_included": False})
    print(json.dumps({"recommendation": "go" if passed else "no-go", "groups": groups}), flush=True)
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    subs = parser.add_subparsers(dest="command", required=True)
    arm = subs.add_parser("run")
    for name in ("arm", "base-url", "source-commit", "artifact-sha256"):
        arm.add_argument("--" + name, required=True)
    arm.add_argument("--copy-window", type=int, choices=(0, 15), required=True)
    arm.add_argument("--reference", action="store_true",
                     help="Unmodified-base greedy reference; absent copy counters stay null.")
    for name in ("api-key-file", "probe", "qualification", "long-fixture", "request-log", "binary", "receipt"):
        arm.add_argument("--" + name, type=Path, required=True)
    summary = subs.add_parser("compare")
    summary.add_argument("receipts", type=Path, nargs=4)
    summary.add_argument("--baseline", type=Path, required=True)
    summary.add_argument("--receipt", type=Path, required=True)
    args = parser.parse_args()
    return run(args) if args.command == "run" else compare(args)


if __name__ == "__main__":
    raise SystemExit(main())
