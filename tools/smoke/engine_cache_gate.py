"""Bounded 32 GiB product-shape startup, C2 and idle-TTFT gate (stdlib only).

Run inside the build/runtime container under the GPU lease. The baseline is Infra's sanitized
server_start record, or {"startup": <that record>}. It is a measured reservation ceiling, not an
estimate chosen by this runner. The server is started twice and always stopped by this runner.
Only numeric measurements, hashes and allowlisted startup fields enter the public receipt.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import secrets
import signal
import statistics
import subprocess
import threading
import time
import urllib.error
import urllib.request

MIB = 1 << 20
SHAPE = {"max_context": 131072, "max_concurrency": 2, "prefill_chunk": 1024,
         "kv_cache": "bf16", "speculative_backend": "dflash2", "speculative_draft_window": 7,
         "cuda_graph": True, "vision": True}
MEMORY_FIELDS = ("device_budget_bytes", "minimum_runtime_reservation_bytes",
                 "runtime_reservation_bytes", "cuda_graph_rebuild_estimate_bytes",
                 "cuda_graph_prepare_increment_bytes", "cuda_graph_calibration_increment_bytes",
                 "cuda_graph_calibration_retained_bytes", "startup_retained_bytes",
                 "available_after_startup_bytes", "kv_capacity_headroom_bytes", "kv_payload_bytes")


class GateError(RuntimeError):
    """A stable, non-private failure reason safe to include in a receipt."""


def require(condition: bool, reason: str) -> None:
    if not condition:
        raise GateError(reason)


def startup_record(path: Path) -> dict:
    records = [json.loads(line) for line in path.read_text().splitlines() if line.strip()]
    starts = [item for item in records if item.get("event") == "server_start"]
    require(len(starts) == 1, "missing_or_duplicate_server_start")
    return starts[0]


def validate_startup(record: dict, baseline: dict, gpu_uuid: str) -> dict:
    engine, memory, environment = (record[key] for key in ("engine", "memory", "environment"))
    require(all(engine.get(key) == value for key, value in SHAPE.items()), "product_shape_changed")
    require(engine.get("kv_capacity_mode") == "auto", "automatic_kv_capacity_not_exercised")
    require(engine.get("kv_capacity", 0) >= SHAPE["max_context"], "logical_context_refused")
    require(engine.get("context_cache", {}).get("device_state_slots") == 2,
            "device_state_slot_shape_changed")
    require(environment.get("gpu_uuid", "").lower() == gpu_uuid.lower(), "selected_gpu_mismatch")
    total = environment.get("total_device_memory_bytes", 0)
    require(31 * 1024 * MIB <= total <= 32 * 1024 * MIB, "not_a_32_gib_device")
    graph = memory.get("cuda_graph_rebuild_estimate_bytes", 0)
    ceiling = baseline["memory"]["cuda_graph_rebuild_estimate_bytes"]
    require(isinstance(graph, int) and 0 < graph <= ceiling, "cuda_graph_reservation_growth")
    budget = memory.get("device_budget_bytes", 0)
    require(0 < budget <= total and 0 < memory.get("runtime_reservation_bytes", 0) <= budget,
            "runtime_reservation_exceeds_device_budget")
    require(memory.get("available_after_startup_bytes", -1) >= 0, "negative_startup_headroom")
    return {"engine": {key: engine[key] for key in (*SHAPE, "kv_capacity_mode", "kv_capacity")},
            "memory": {key: memory[key] for key in MEMORY_FIELDS if key in memory},
            "total_device_memory_bytes": total}


def metrics(base_url: str, key: str) -> dict[str, float]:
    request = urllib.request.Request(base_url + "/metrics", headers={"Authorization": "Bearer " + key})
    with urllib.request.urlopen(request, timeout=10) as response:
        text = response.read().decode()
    return {line.split()[0]: float(line.split()[1]) for line in text.splitlines()
            if line and not line.startswith("#") and "{" not in line}


def stream(base_url: str, key: str, label: str, *, outputs: int = 32, previous: str | None = None,
           store: bool = False, barrier: threading.Barrier | None = None) -> dict:
    # The first differing text precedes the long body, so repeated idle samples do not benchmark
    # an exact cached long prefix. Off/on arms use byte-identical inputs for each sample label.
    text = label + ": " + "Explain fair scheduling and independent cancellation. " * 256
    payload = {"model": "engine-cache-gate", "input": text, "stream": True, "store": store,
               "temperature": 0, "max_output_tokens": outputs,
               "prompt_cache_key": "engine-cache-gate-session"}
    if previous is not None:
        payload["previous_response_id"] = previous
    request = urllib.request.Request(base_url + "/v1/responses", data=json.dumps(payload).encode(),
                                     headers={"Authorization": "Bearer " + key,
                                              "Content-Type": "application/json"})
    if barrier is not None:
        barrier.wait(timeout=10)
    submitted = time.monotonic()
    first = None
    terminal = None
    hashes = {kind: hashlib.sha256() for kind in ("output_text", "reasoning_text")}
    with urllib.request.urlopen(request, timeout=180) as response:
        require(response.headers.get_content_type() == "text/event-stream", "not_an_sse_response")
        data = []
        for raw in response:
            line = raw.decode().rstrip("\r\n")
            if line.startswith("data:"):
                data.append(line[5:].lstrip())
            elif not line and data:
                encoded = "\n".join(data)
                data = []
                if encoded == "[DONE]":
                    break
                event = json.loads(encoded)
                kind = event.get("type", "")
                for channel, digest in hashes.items():
                    if kind == "response." + channel + ".delta" and event.get("delta"):
                        if first is None:
                            first = time.monotonic()
                        digest.update(event["delta"].encode())
                if kind in ("response.completed", "response.incomplete"):
                    terminal = event["response"]
                    break
                require(kind not in ("response.failed", "response.cancelled", "error"),
                        "generation_refused_or_failed")
    ended = time.monotonic()
    require(terminal is not None and first is not None, "stream_missing_output_or_terminal")
    usage = terminal.get("usage") or {}
    require(0 < usage.get("output_tokens", 0) <= outputs, "invalid_output_accounting")
    return {"label": label, "id": terminal["id"], "submitted_s": submitted, "first_output_s": first,
            "completed_s": ended, "ttft_s": first - submitted, "total_s": ended - submitted,
            "input_tokens": usage.get("input_tokens"), "output_tokens": usage["output_tokens"],
            "cached_tokens": (usage.get("input_tokens_details") or {}).get("cached_tokens", 0),
            **{channel + "_sha256": digest.hexdigest() for channel, digest in hashes.items()}}


def c2_siblings(base_url: str, key: str) -> dict:
    seed = stream(base_url, key, "fanout seed", store=True)
    before = metrics(base_url, key)
    barrier = threading.Barrier(2)
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        older = pool.submit(stream, base_url, key, "long sibling", outputs=192,
                            previous=seed["id"], store=True, barrier=barrier)
        younger = pool.submit(stream, base_url, key, "short sibling", outputs=48,
                              previous=seed["id"], store=True, barrier=barrier)
        short = younger.result()
        continued = stream(base_url, key, "continue short sibling", outputs=16,
                           previous=short["id"], store=True)
        long = older.result()
    after = metrics(base_url, key)
    rows = after["ninfer_decode_row_rounds_total"] - before["ninfer_decode_row_rounds_total"]
    rounds = after["ninfer_decode_rounds_total"] - before["ninfer_decode_rounds_total"]
    require(rows > rounds > 0, "no_multirow_decode")
    require(short["completed_s"] < long["completed_s"] and
            continued["submitted_s"] < long["completed_s"], "sibling_continuation_was_serialized")
    return {"decode_row_rounds": rows, "decode_rounds": rounds,
            "requests": [seed, long, short, continued]}


def stop(process: subprocess.Popen) -> None:
    if process.poll() is None:
        os.killpg(process.pid, signal.SIGTERM)
        try:
            process.wait(timeout=15)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=10)


def run_arm(args: argparse.Namespace, baseline: dict, grace_ms: int, root: Path) -> dict:
    root.mkdir()
    events = root / "requests.jsonl"
    key = secrets.token_urlsafe(24)
    command = [str(args.binary), str(args.artifact), "--host", "127.0.0.1", "--port", str(args.port),
               "--api-key", key, "--model-id", "engine-cache-gate", "--device", str(args.device),
               "--max-context", "131072", "--kv-capacity", "auto", "--max-concurrency", "2",
               "--prefill-chunk", "1024", "--kv-dtype", "bf16", "--spec", "dflash2",
               "--draft-tokens", "7", "--lm-head-draft", "--vision", "--preserve-thinking",
               "--device-state-slots", "2", "--host-context-mib", args.host_context_mib,
               "--gpu-keep-warm-ms", str(grace_ms), "--request-log-jsonl", str(events)]
    base_url = "http://127.0.0.1:" + str(args.port)
    with (root / "server.log").open("wb") as log:
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            deadline = time.monotonic() + args.startup_timeout
            while time.monotonic() < deadline:
                require(process.poll() is None, "server_exited_during_startup")
                try:
                    with urllib.request.urlopen(base_url + "/health", timeout=1) as health:
                        if json.load(health) == {"status": "ok"}:
                            break
                except (OSError, urllib.error.URLError):
                    pass
                time.sleep(0.25)
            else:
                raise GateError("startup_deadline_exceeded")
            startup = validate_startup(startup_record(events), baseline, args.gpu_uuid)
            stream(base_url, key, "prime clocks")
            samples = []
            for sample in range(args.samples):
                time.sleep(args.idle_seconds)
                samples.append(stream(base_url, key, "idle sample " + str(sample)))
            siblings = c2_siblings(base_url, key)
            require(metrics(base_url, key).get("ninfer_engine_ready") == 1, "engine_unavailable_after_c2")
            return {"gpu_keep_warm_ms": grace_ms, "startup": startup, "idle_ttft": samples,
                    "median_idle_ttft_s": statistics.median(item["ttft_s"] for item in samples),
                    "c2_siblings": siblings}
        finally:
            stop(process)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--artifact", type=Path, required=True)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--source-commit", required=True)
    parser.add_argument("--gpu-uuid", required=True)
    parser.add_argument("--device", type=int, default=0)
    parser.add_argument("--host-context-mib", required=True)
    parser.add_argument("--port", type=int, default=18150)
    parser.add_argument("--idle-seconds", type=float, default=12)
    parser.add_argument("--samples", type=int, default=3)
    parser.add_argument("--startup-timeout", type=float, default=300)
    parser.add_argument("--work-dir", type=Path, required=True,
                        help="private scratch directory retaining server logs for diagnosis")
    parser.add_argument("--receipt", type=Path, required=True)
    args = parser.parse_args()
    require(18150 <= args.port <= 18159, "port_outside_E1_block")
    require(args.samples > 0 and 9 <= args.idle_seconds < 60, "invalid_idle_measurement_bounds")
    require(len(args.source_commit) == 40 and all(c in "0123456789abcdef" for c in args.source_commit),
            "source_commit_not_full_sha")
    baseline_bytes = args.baseline.read_bytes()
    document = json.loads(baseline_bytes)
    baseline = document.get("startup", document)
    validate_startup(baseline, baseline, args.gpu_uuid)
    receipt = {"artifact_type": "ninfer_engine_cache_gate", "schema_version": 1,
               "source_commit": args.source_commit, "baseline_sha256": hashlib.sha256(baseline_bytes).hexdigest(),
               "shape": SHAPE, "idle_seconds": args.idle_seconds, "arms": [], "passed": False}
    with args.binary.open("rb") as binary:
        receipt["binary_sha256"] = hashlib.file_digest(binary, "sha256").hexdigest()
    try:
        args.work_dir.mkdir(parents=True, exist_ok=True)
        for grace in (0, 60000):
            receipt["arms"].append(run_arm(args, baseline, grace, args.work_dir / str(grace)))
        off, on = receipt["arms"]
        receipt["median_idle_ttft_ratio_on_over_off"] = on["median_idle_ttft_s"] / off["median_idle_ttft_s"]
        receipt["passed"] = True
    except Exception as error:
        # Raw server/HTTP/OS diagnostics can contain paths, request text or credentials.
        receipt["failure"] = str(error) if isinstance(error, GateError) else type(error).__name__
    args.receipt.parent.mkdir(parents=True, exist_ok=True)
    args.receipt.write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps({"passed": receipt["passed"], "failure": receipt.get("failure"),
                      "median_idle_ttft_ratio_on_over_off": receipt.get("median_idle_ttft_ratio_on_over_off")}))
    return 0 if receipt["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
