#!/usr/bin/env python3
"""Run only inside the exclusive GPU lease. Raw requests/outputs remain in --workdir.

This local tracer uses Chat history, not the process-local Responses store. It captures a
native input recovery point, restores fresh native owners, and compares the next turn to
the uninterrupted process. The receipt contains hashes/counters, never model output.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import shlex
import subprocess
import time
import urllib.error
import urllib.request


def sha_file(path):
    with open(path, "rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def sha(data):
    return hashlib.sha256(data).hexdigest()


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode()


def command(args):
    return subprocess.check_output(args, stderr=subprocess.STDOUT, text=True).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workdir", type=Path, required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--model-sha256", required=True)
    parser.add_argument("--source-commit", required=True)
    parser.add_argument("--image", default="ov-build-env:master-64d10fbf")
    args = parser.parse_args()
    os.umask(0o077)
    args.workdir.mkdir(parents=True, exist_ok=False)
    root = args.workdir.resolve()
    bearer = secrets.token_hex(32)
    (root / "bearer").write_text(bearer)
    (root / "origin.key").write_bytes(secrets.token_bytes(32))
    receipt = {"artifact_type": "ninfer_upstream_durable_tracer", "schema_version": 1,
               "verdict": "blocked", "method": "local rehearsal; Chat full-history continuation",
               "upstream_commit": "64d10fbf",
               "tracer_source_commit": args.source_commit, "backend": "dflash2",
               "gpu_lease_priority": 2, "long_prefix_minimum_tokens": 16384,
               "responses_retained_store": "not ported; explicitly outside this narrow tracer",
               "raw_evidence_directory": root.name, "gpu_jobs": [], "processes": []}
    active = None
    base_url = "http://127.0.0.1:18120"
    config = ["--host", "0.0.0.0", "--port", "18120", "--model-id", "q38-tracer",
              "--max-context", "32768", "--kv-capacity", "32768", "--prefill-chunk", "1024",
              "--kv-dtype", "bf16", "--max-concurrency", "1", "--device-state-slots", "2",
              "--host-context-mib", "512", "--spec", "dflash2", "--draft-tokens", "7",
              "--lm-head-draft", "--no-thinking", "--preserve-thinking", "--greedy", "--seed", "42"]

    def request(body, label):
        raw_request = canonical(body)
        (root / (label + "-request.json")).write_bytes(raw_request)
        req = urllib.request.Request(base_url + "/v1/chat/completions", raw_request,
                                     {"Content-Type": "application/json", "Authorization": "Bearer " + bearer})
        started = time.monotonic()
        try:
            with urllib.request.urlopen(req, timeout=1200) as response:
                raw = response.read()
        except urllib.error.HTTPError as error:
            (root / (label + "-error.json")).write_bytes(error.read())
            raise RuntimeError(f"{label}: HTTP {error.code}; private error body retained") from None
        (root / (label + "-response.json")).write_bytes(raw)
        result = json.loads(raw)
        receipt[label] = {"elapsed_seconds": time.monotonic() - started,
                          "request_sha256": sha(raw_request), "usage": result.get("usage"),
                          "message_sha256": sha(canonical(result["choices"][0]["message"]))}
        return result

    def start(mode):
        nonlocal active
        active = "ov-A2-tracer-" + mode
        serve = "exec /build/apps/ninfer-serve /models/model.ninfer " + shlex.join(config)
        serve += " --api-key \"$(cat /private/bearer)\" --request-log-jsonl /private/" + mode + "-requests.jsonl"
        docker = ["docker", "run", "-d", "--name", active, "--gpus", "all",
                  "--user", f"{os.getuid()}:{os.getgid()}",
                  "-p", "127.0.0.1:18120:18120", "-v", str(args.build.resolve()) + ":/build:ro",
                  "-v", str(args.model.resolve()) + ":/models/model.ninfer:ro", "-v", str(root) + ":/private",
                  "-e", "NINFER_TRACER_MODE=" + mode, "-e", "NINFER_TRACER_ROOT=/private/checkpoint",
                  "-e", "NINFER_TRACER_KEY_FILE=/private/origin.key",
                  "-e", "NINFER_TRACER_IDENTITY=" + canonical(receipt["identity"]).decode(),
                  "--entrypoint", "bash", args.image, "-lc", serve]
        container_id = command(docker)
        start_time = time.monotonic()
        while time.monotonic() - start_time < 600:
            try:
                with urllib.request.urlopen(base_url + "/health", timeout=3) as response:
                    if response.status == 200:
                        state = json.loads(command(["docker", "inspect", active, "--format", "{{json .State}}"]));
                        receipt["processes"].append({"mode": mode, "container_id": container_id,
                                                     "host_pid": state["Pid"], "started_at": state["StartedAt"],
                                                     "startup_seconds": time.monotonic() - start_time})
                        if "cuda_toolkit" not in receipt:
                            receipt["cuda_toolkit"] = command(["docker", "exec", active, "nvcc", "--version"])
                        return
            except (OSError, urllib.error.URLError):
                pass
            if command(["docker", "inspect", active, "--format", "{{.State.Running}}"] ) != "true":
                state = json.loads(command(["docker", "inspect", active, "--format", "{{json .State}}"]))
                receipt["startup_failure"] = {"mode": mode, "container_id": container_id,
                                              "exit_code": state["ExitCode"], "oom_killed": state["OOMKilled"],
                                              "started_at": state["StartedAt"], "finished_at": state["FinishedAt"]}
                raise RuntimeError(f"{mode} server exited before health")
            time.sleep(1)
        raise RuntimeError(f"{mode} health timeout")

    def stop():
        nonlocal active
        if active is None:
            return
        try:
            collected = subprocess.run(["docker", "logs", active], stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT, text=True, check=False)
            logs = collected.stdout
            (root / (active + ".log")).write_text(logs)
            for line in logs.splitlines():
                if line.startswith("durable-tracer "):
                    receipt.setdefault("native_events", []).append(line)
        finally:
            subprocess.run(["docker", "stop", "-t", "15", active], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, check=False)
            subprocess.run(["docker", "rm", active], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, check=False)
            active = None

    try:
        actual_model = sha_file(args.model)
        if actual_model != args.model_sha256:
            raise RuntimeError("model digest mismatch")
        receipt["identity"] = {"model_sha256": actual_model,
                               "binary_sha256": sha_file(args.build / "apps/ninfer-serve"),
                               "launcher_sha256": sha_file(Path(__file__)),
                               "seccomp_policy": "Docker default; no override",
                               "container_user": f"{os.getuid()}:{os.getgid()}",
                               "image_id": command(["docker", "image", "inspect", args.image, "--format", "{{.Id}}"]),
                               "config_sha256": sha(canonical(config)), "source_commit": args.source_commit}
        receipt["gpu"] = command(["/usr/lib/wsl/lib/nvidia-smi", "--query-gpu=name,uuid,driver_version,memory.total",
                                   "--format=csv,noheader"])
        receipt["config"] = config
        long_text = "alpha beta gamma delta epsilon zeta eta theta\n" * 2200
        first = {"model": "q38-tracer", "temperature": 0, "seed": 42, "max_tokens": 32,
                 "enable_thinking": False, "preserve_thinking": True,
                 "messages": [{"role": "user", "content": long_text + "\nReply with READY and nothing else."}]}
        start("export")
        first_response = request(first, "first_turn")
        checkpoint = json.loads((root / "checkpoint/manifest.json").read_text())
        receipt["checkpoint"] = {"frontier_tokens": checkpoint["frontier"],
                                 "payload_bytes": sum(f["bytes"] for f in checkpoint["files"].values()),
                                 "file_count": len(checkpoint["files"]),
                                 "manifest_sha256": sha_file(root / "checkpoint/manifest.json"),
                                 "mac_present": (root / "checkpoint/manifest.mac").is_file()}
        if checkpoint["frontier"] < 16384:
            raise RuntimeError("checkpoint did not reach 16K tokens")
        assistant = first_response["choices"][0]["message"]
        history_assistant = {k: assistant[k] for k in ("role", "content", "reasoning_content", "tool_calls") if k in assistant}
        second = dict(first, max_tokens=64, messages=first["messages"] + [history_assistant,
                         {"role": "user", "content": "Give the eight Greek letter names from the record in reverse order."}])
        control = request(second, "uninterrupted_control")
        stop()
        start("import")
        restored = request(second, "restored_continuation")
        stop()
        if (len(receipt["processes"]) != 2 or
                receipt["processes"][0]["container_id"] == receipt["processes"][1]["container_id"] or
                receipt["processes"][0]["host_pid"] == receipt["processes"][1]["host_pid"]):
            raise RuntimeError("restart did not establish distinct server processes")
        receipt["real_process_restart"] = True
        control_message = canonical(control["choices"][0]["message"])
        restored_message = canonical(restored["choices"][0]["message"])
        receipt["byte_identical"] = control_message == restored_message
        receipt["compared_bytes"] = len(control_message)
        for mode in ("export", "import"):
            done = [json.loads(line) for line in (root / (mode + "-requests.jsonl")).read_text().splitlines()
                    if line.strip()]
            done = [r for r in done if r.get("event") == "request_done"]
            receipt[mode + "_requests"] = [{"result": {k: r["result"][k] for k in
                    ("prompt_tokens", "completion_tokens", "computed_prefill_tokens", "prefix_cache_hit_tokens", "prefix_reuse_path")},
                    "timings_seconds": r["timings_seconds"], "speculative": r["speculative"]} for r in done]
        imported = receipt["import_requests"]
        if len(imported) != 1 or imported[0]["result"]["prefix_cache_hit_tokens"] < 16384:
            raise RuntimeError("restored request did not select the imported long checkpoint")
        import_events = [e for e in receipt.get("native_events", []) if e.startswith("durable-tracer import ")]
        if len(import_events) != 1:
            raise RuntimeError("missing native import evidence")
        receipt["restore_seconds"] = float(re.search(r"seconds=([0-9.e+-]+)", import_events[0])[1])
        if not receipt["byte_identical"] or not control["choices"][0]["message"].get("content"):
            raise RuntimeError("continuation is empty or not byte-identical")
        receipt["verdict"] = "pass"
    except Exception as error:
        (root / "failure-private.txt").write_text(str(error))
        receipt["failure"] = {"type": type(error).__name__,
                              "detail": "failure-private.txt"}
    finally:
        stop()
        (root / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps({"verdict": receipt["verdict"], "receipt": str(root / "receipt.json"),
                      "failure": receipt.get("failure"), "byte_identical": receipt.get("byte_identical")}))
    return 0 if receipt["verdict"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
