#!/usr/bin/env python3
"""EXP-098 native-only measurement; never changes an installed runtime/profile.

Run fit first, then mtp/dflash2 in separately granted windows. Inputs stay private;
receipts contain numeric observations, identifiers and hashes, never response text.
The leak arm is EXP-085's 504-prompt redaction subset, not its full quality screen.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
import os
import secrets
import socket
import subprocess
import sys
import threading
import time
import urllib.request
from pathlib import Path

from run_exp097 import aggregate, bind_server, digest, load_module, records

MODEL_SHA = "81f924d440c27261d820c19a9f8d45794c5aee410f8a68bd358133fa8c0375da"
CORPUS_SHA = "7be1bd85dca30c3689ed351d6a60546b46ed6c430269d0786e28b551778a7617"
VACUOUS = {"struct-001", "struct-003", "struct-004", "struct-006", "struct-007"}
LANES = {
    "rtx4090": {"gpu": 0, "port": 18208, "headroom": 640, "prefill": 2048,
                "slots": 2, "host_mib": "14787.734375"},
    "rtx3090": {"gpu": 1, "port": 18218, "headroom": 768, "prefill": 1024,
                "slots": 4, "host_mib": "11715.734375"},
}


def sha_file(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(4 * 1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def save(path, value):
    text = json.dumps(value, indent=2, allow_nan=False) + "\n"
    if any(p in text for p in ("/home/", "/Users/", "C:\\\\Users\\\\", "sf-old", "sf-pc", "nyc-pc")):
        raise ValueError("private marker in public receipt")
    path.write_text(text, encoding="utf-8")


def gpu_sample(gpu):
    result = subprocess.run([
        "nvidia-smi", "-i", str(gpu),
        "--query-gpu=memory.total,memory.used,memory.free,temperature.gpu,power.draw",
        "--format=csv,noheader,nounits"], capture_output=True, text=True,
        timeout=15, check=True)
    values = result.stdout.strip().split(",")
    return dict(zip(("total_mib", "used_mib", "free_mib", "temperature_c", "power_w"),
                    (float(v.strip()) for v in values)))


def summarize(rows):
    result = aggregate(rows)
    ok = [r for r in rows if r.get("http_status") == 200 and "server" in r]
    drafted = sum(r["server"]["drafted_tokens"] for r in ok)
    accepted = sum(r["server"]["accepted_tokens"] for r in ok)
    result.update(drafted_tokens=drafted, accepted_tokens=accepted,
                  acceptance_rate=accepted / drafted if drafted else None,
                  tokens_per_round=result.get("completion_tokens", 0) / result["rounds"]
                  if result.get("rounds") else None)
    return result


class Server:
    def __init__(self, args, backend, context, label):
        self.args, self.backend, self.context = args, backend, context
        self.lane = LANES[args.lane]
        self.directory = args.work / label
        self.directory.mkdir(parents=True, exist_ok=False)
        self.log = self.directory / "requests.jsonl"
        self.key = secrets.token_hex(32)
        key_path = self.directory / "api.key"
        key_path.write_text(self.key, encoding="ascii")
        os.chmod(key_path, 0o600)
        self.url = "http://127.0.0.1:" + str(self.lane["port"])
        self.process = None
        self.started = False
        self.samples = []
        self.stop_sampling = threading.Event()
        self.stdout = (self.directory / "stdout.log").open("wb")
        self.stderr = (self.directory / "stderr.log").open("wb")
        self.command = [str(args.binary), str(args.model), "--host", "127.0.0.1",
            "--port", str(self.lane["port"]), "--api-key-file", str(key_path),
            "--model-id", "q38-ninfer", "--device", str(self.lane["gpu"]),
            "--max-context", str(context), "--kv-capacity", str(context),
            "--admission-headroom-mib", str(self.lane["headroom"]),
            "--max-concurrency", "1", "--prefill-chunk", str(self.lane["prefill"]),
            "--device-state-slots", str(self.lane["slots"]),
            "--host-context-mib", self.lane["host_mib"], "--kv-dtype", "int8",
            "--spec", backend, "--draft-tokens", "7" if backend == "dflash2" else "3",
            "--request-log-jsonl", str(self.log), "--log-stats-interval-ms", "0"]
        if backend == "mtp":
            self.command.append("--lm-head-draft")
        self.receipt = {"label": label, "backend": backend, "context": context,
                        "before": gpu_sample(self.lane["gpu"])}

    def request(self, route, payload=None):
        data = None if payload is None else json.dumps(payload).encode()
        req = urllib.request.Request(self.url + route, data=data, headers={
            "Authorization": "Bearer " + self.key, "Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=120) as response:
            return json.load(response)

    def sample_loop(self):
        while not self.stop_sampling.wait(2):
            try:
                self.samples.append(gpu_sample(self.lane["gpu"]))
            except (OSError, ValueError, subprocess.SubprocessError):
                self.receipt["sampler_errors"] = self.receipt.get("sampler_errors", 0) + 1

    def start(self):
        with socket.socket() as sock:
            if sock.connect_ex(("127.0.0.1", self.lane["port"])) == 0:
                raise RuntimeError("reserved experiment port occupied")
        env = os.environ.copy()
        env["CUDA_DEVICE_ORDER"] = "PCI_BUS_ID"
        env.pop("CUDA_VISIBLE_DEVICES", None)
        env.pop("NINFER_VRAM_BUDGET_MIB", None)
        started = time.monotonic()
        self.process = subprocess.Popen(self.command, stdin=subprocess.DEVNULL,
            stdout=self.stdout, stderr=self.stderr, env=env, cwd=self.args.binary.parent)
        self.deadman = threading.Timer(max(0, self.args.deadline - time.monotonic()),
                                       self.process.kill)
        self.deadman.daemon = True
        self.deadman.start()
        self.sampler = threading.Thread(target=self.sample_loop, daemon=True)
        self.sampler.start()
        while time.monotonic() - started < 240 and self.process.poll() is None:
            try:
                self.request("/health")
                self.started = True
                break
            except (OSError, ValueError):
                time.sleep(1)
        self.receipt.update(ready=self.started, startup_wall_s=time.monotonic() - started)
        if self.started:
            start = next(r for r in records(self.log) if r.get("event") == "server_start")
            engine = start["engine"]
            self.receipt["engine"] = {k: engine[k] for k in (
                "device", "max_context", "kv_capacity", "max_concurrency", "prefill_chunk",
                "kv_cache", "cuda_graph", "speculative_backend", "speculative_draft_window",
                "proposal_head", "context_cache")}
            self.receipt["memory"] = start["memory"]
            self.receipt["ready_vram"] = gpu_sample(self.lane["gpu"])
            self.receipt["spare_after_headroom_bytes"] = (
                start["memory"]["available_after_startup_bytes"] - self.lane["headroom"] * 1048576)
        return self.started

    def close(self):
        if hasattr(self, "deadman"):
            self.deadman.cancel()
        if self.process is not None and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=20)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=20)
        self.stop_sampling.set()
        if hasattr(self, "sampler"):
            self.sampler.join(timeout=20)
        self.stdout.close()
        self.stderr.close()
        if self.process is not None:
            self.receipt["exit_code"] = self.process.returncode
        self.receipt["owned_server_stopped"] = self.process is None or self.process.poll() is not None
        self.receipt["after"] = gpu_sample(self.lane["gpu"])
        if self.samples:
            self.receipt["sampled_peak_used_mib"] = max(s["used_mib"] for s in self.samples)
            self.receipt["sampled_min_free_mib"] = min(s["free_mib"] for s in self.samples)
            self.receipt["vram_sample_count"] = len(self.samples)
        raw = (self.directory / "stderr.log").read_bytes()
        self.receipt["stderr_sha256"] = hashlib.sha256(raw).hexdigest()
        text = raw.decode("utf-8", errors="replace")
        categories = [name for needle, name in (
            ("device budget cannot hold", "startup_memory_budget"),
            ("out of memory", "cuda_out_of_memory"),
            ("illegal memory", "cuda_illegal_memory_access"), ("not supported", "unsupported"),
            ("no kernel image", "no_kernel_image")) if needle in text.lower()]
        self.receipt["diagnostic_categories"] = categories
        (self.directory / "api.key").unlink(missing_ok=True)


def make_role_args(q, args, server):
    a = q.parse_args(["--model", "q38-ninfer", "--base-url", server.url + "/v1",
        "--cases", str(args.qualification / "cases"), "--out", "unused", "--concurrency", "1",
        "--reasoning-mode", "low", "--reasoning-wire", "top-level",
        "--reasoning-token-headroom", "0", "--timeout", "900", "--no-ssh"])
    a.api_key = server.key
    return a


def measure(args, server, selected, q, scorer, kind):
    rows = []
    role_args = make_role_args(q, args, server)
    start_ms = int(time.time() * 1000)
    for case in selected:
        if time.monotonic() >= args.deadline:
            raise TimeoutError("window deadline")
        submitted = int(time.time() * 1000)
        warnings = []
        rec = q.execute_case(case, role_args, str(args.qualification / "fixtures"),
            server.url + "/v1/chat/completions", not rows, warnings, None)
        if warnings:
            raise RuntimeError("case source resolution warning")
        tools = [{k: v for k, v in t.items() if k != "id"} for t in rec["tool_calls"]]
        usage = rec["usage"]
        row = {"id": case["case_id"], "workload": case["class"], "kind": kind,
            "submitted_unix_ms": submitted, "case_sha256": rec["case_sha256"],
            "request_sha256": rec["request_sha256"], "attempts": rec["attempt"],
            "http_status": 200 if usage.get("completion_tokens") is not None else None,
            "prompt_tokens": usage.get("prompt_tokens"), "completion_tokens": usage.get("completion_tokens"),
            "client_verdict": (rec["error"] or "answered").split(":", 1)[0],
            "wall_ms": rec["wall_ms"], "ttft_ms": rec["ttft_ms"],
            "output_sha256": digest([rec["content"], rec["reasoning"], tools])}
        if case.get("safety", {}).get("contains_synthetic_secret"):
            score = scorer.score_case(case, rec)
            row["leaks"] = int(score["secret_leaks"])
            row["pass"] = row["leaks"] == 0
            row["error"] = bool(score["error"])
        rows.append(row)
        # Incremental numeric-only evidence survives a preempted window.
        save(server.directory / (kind + ".json"), rows)
        print(kind, case["case_id"], row["client_verdict"], flush=True)
    time.sleep(2)
    bind_server(server.log, rows, start_ms)
    save(server.directory / (kind + ".json"), rows)
    return rows


def fit(args, receipt):
    # 131072 on the 4090 is already known not to fit; do not repeat that observation.
    low, high = 0, 2048
    attempts = []
    candidate = 1024 if args.lane == "rtx4090" else 2048
    while True:
        if time.monotonic() >= args.deadline:
            raise TimeoutError("window deadline")
        context = candidate * 64
        server = Server(args, "dflash2", context, "fit-" + str(context))
        try:
            ready = server.start()
            if ready:
                response = server.request("/v1/chat/completions", {
                    "model": "q38-ninfer", "messages": [{"role": "user", "content": "Count from one to twenty."}],
                    "temperature": 0, "max_tokens": 64})
                server.receipt["decode_probe"] = {"completion_tokens": response.get("usage", {}).get("completion_tokens"),
                    "output_sha256": digest(response.get("choices", []))}
                ready = bool(server.receipt["decode_probe"]["completion_tokens"])
        except (OSError, ValueError) as error:
            server.receipt["probe_error_type"] = type(error).__name__
            ready = False
        finally:
            server.close()
        if ready and server.receipt["spare_after_headroom_bytes"] < 0:
            ready = False
            server.receipt["diagnostic_categories"].append("post_startup_headroom_shortfall")
        server.receipt["usable_with_headroom"] = ready
        attempts.append(server.receipt)
        receipt["fit_attempts"] = attempts
        save(args.out, receipt)
        if ready:
            low = candidate
        else:
            # Non-memory initialization/execution failures are not fit boundaries.
            if not server.receipt["diagnostic_categories"] or any(c not in (
                    "startup_memory_budget", "cuda_out_of_memory", "post_startup_headroom_shortfall")
                    for c in server.receipt["diagnostic_categories"]):
                receipt["status"] = "execution_or_initialization_failure"
                receipt["fitting_context"] = None
                break
            high = candidate
        if high - low <= 1 or (ready and candidate == 2048):
            receipt["status"] = "fit_boundary_measured" if low else "no_fitting_page"
            receipt["fitting_context"] = low * 64
            receipt["next_unfit_context"] = None if low == 2048 else high * 64
            break
        candidate = (low + high) // 2
    receipt["context_search_quantum_tokens"] = 64
    receipt["context_search_upper_bound"] = 131072
    receipt["upper_bound_prior"] = "assignment: 4090 131072 does not fit" if args.lane == "rtx4090" else None


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--lane", choices=LANES, required=True)
    p.add_argument("--mode", choices=("fit", "mtp", "dflash2"), required=True)
    p.add_argument("--binary", type=Path, required=True)
    p.add_argument("--model", type=Path, required=True)
    p.add_argument("--source", required=True)
    p.add_argument("--qualification", type=Path, required=True)
    p.add_argument("--redaction-screen", type=Path, required=True)
    p.add_argument("--work", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--context", type=int)
    p.add_argument("--grant-id", required=True)
    p.add_argument("--minutes", type=int, required=True)
    args = p.parse_args()
    args.work.mkdir(parents=True, exist_ok=False)
    args.deadline = time.monotonic() + args.minutes * 60
    receipt = {"artifact_type": "ninfer_exp098_native_measurement", "schema_version": 1,
        "experiment": "EXP-098", "lane": args.lane, "mode": args.mode,
        "source_commit": args.source, "binary_sha256": sha_file(args.binary),
        "model_sha256": sha_file(args.model), "model_bytes": args.model.stat().st_size,
        "runner_sha256": sha_file(Path(__file__)), "grant_id": args.grant_id,
        "grant_minutes": args.minutes, "lane_shape": LANES[args.lane],
        "started_unix_s": time.time(), "raw_prompts_outputs_or_secrets_included": False}
    if receipt["model_sha256"] != MODEL_SHA:
        raise ValueError("published v3 model identity mismatch")
    if args.mode == "fit":
        fit(args, receipt)
    else:
        if not args.context:
            raise ValueError("measured fitting context required")
        q = load_module("exp098_qualification", args.qualification / "scripts/run_qualification.py")
        scorer = load_module("exp098_scorer", args.qualification / "scripts/score_qualification.py")
        redaction = load_module("exp098_redaction", args.redaction_screen)
        cases, _ = q.load_cases([str(args.qualification / "cases")])
        if q.corpus_sha256([q.case_sha256(c) for c in cases]) != CORPUS_SHA or len(cases) != 89:
            raise ValueError("frozen corpus identity mismatch")
        roles = [c for c in cases if c["case_id"] not in VACUOUS]
        controls = sorted((c for c in roles if c.get("safety", {}).get("contains_synthetic_secret")), key=lambda c: c["case_id"])
        if len(roles) != 84 or len(controls) != 7:
            raise ValueError("unexpected role/control count")
        variants = []
        for case in controls:
            for i, prefix in enumerate(redaction.variant_prefixes(72)):
                variant = copy.deepcopy(case)
                variant["case_id"] += ".v" + str(i)
                variant["input"]["task"] = prefix + variant["input"]["task"]
                variants.append(variant)
        receipt["qualification_runner_sha256"] = sha_file(args.qualification / "scripts/run_qualification.py")
        receipt["scorer_sha256"] = sha_file(args.qualification / "scripts/score_qualification.py")
        receipt["corpus_sha256"] = CORPUS_SHA
        receipt["leak_screen"] = {"prompts": 504, "variants_per_control": 72,
            "screen_sha256": digest(variants), "scope": "EXP-085 redaction subset only",
            "determinism": "v0 and v1 of each control on a fresh process (EXP-063 protocol)"}
        server = Server(args, args.mode, args.context, args.mode + "-screen")
        try:
            if not server.start():
                raise RuntimeError("matched arm failed to initialize")
            receipt["roles"] = measure(args, server, roles, q, scorer, "roles")
            receipt["redaction"] = measure(args, server, variants, q, scorer, "redaction")
        finally:
            server.close()
            receipt["server"] = server.receipt
            save(args.out, receipt)
        server = Server(args, args.mode, args.context, args.mode + "-determinism")
        try:
            if not server.start():
                raise RuntimeError("determinism arm failed to initialize")
            subset = [c for c in variants if c["case_id"].rsplit(".v", 1)[1] in ("0", "1")]
            receipt["determinism"] = measure(args, server, subset, q, scorer, "determinism")
        finally:
            server.close()
            receipt["determinism_server"] = server.receipt
            save(args.out, receipt)
        receipt["role_summary"] = summarize(receipt["roles"])
        receipt["workloads"] = {w: summarize([r for r in receipt["roles"] if r["workload"] == w])
            for w in sorted({r["workload"] for r in receipt["roles"]})}
        receipt["status"] = "measured"
    receipt["finished_unix_s"] = time.time()
    save(args.out, receipt)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
