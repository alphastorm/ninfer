#!/usr/bin/env python3
"""Real request normalization and connected-stream Stop, followed by durable native resume."""
from __future__ import annotations

import argparse
import hashlib
import http.client
import json
import os
from pathlib import Path
import re
import secrets
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.request

from serve_real_process import ManagedProcess
from test_serve_restore_real import build_provenance, digest_file, require


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--artifact", type=Path, default=os.environ.get("NINFER_TEST_ARTIFACT"))
    parser.add_argument("--artifact-sha256", default=os.environ.get("NINFER_TEST_ARTIFACT_SHA256"))
    parser.add_argument("--receipt", type=Path)
    args = parser.parse_args()
    if args.artifact is None:
        print("SKIP: explicit NINFER_TEST_ARTIFACT required")
        return 77
    require(args.server.is_file() and args.artifact.is_file(), "binary/artifact missing")
    require(re.fullmatch(r"[0-9a-f]{64}", args.artifact_sha256 or "") is not None,
            "explicit artifact SHA-256 required")
    version = subprocess.run([str(args.server), "--version"], check=True,
                             capture_output=True, text=True).stdout
    source, upstream = build_provenance(version)
    binary_sha = digest_file(args.server)
    receipt = {"schema": "ninfer-serve-resilience/1", "source": source, "upstream": upstream,
               "binary_sha256": binary_sha, "artifact_sha256": args.artifact_sha256}
    with tempfile.TemporaryDirectory(prefix="ninfer-serve-resilience-") as temporary:
        root = Path(temporary)
        key = secrets.token_hex(32)
        key_file = root / "api.key"
        key_file.write_text(key, encoding="ascii")
        key_file.chmod(0o600)
        cache_key = secrets.token_hex(16)
        session = hashlib.sha256(b"ninfer:prompt_cache_key:v1\0" + cache_key.encode()).hexdigest()
        checkpoints = root / "checkpoints"
        report = root / "shutdown.json"
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        endpoint = f"http://127.0.0.1:{port}"
        def request(method, route, body=None, session_header=False, text=False):
            headers = {"Authorization": "Bearer " + key}
            if session_header: headers["X-NInfer-Session"] = session
            if body is not None: headers["Content-Type"] = "application/json"
            req = urllib.request.Request(endpoint + route, method=method, headers=headers,
                data=None if body is None else json.dumps(body).encode())
            try:
                with urllib.request.urlopen(req, timeout=120) as response:
                    return response.status, response.read().decode() if text else json.load(response)
            except urllib.error.HTTPError as error:
                return error.code, json.load(error)
        argv = [str(args.server), str(args.artifact), "--host", "127.0.0.1", "--port", str(port),
                "--model-id", "serve-resilience", "--api-key-file", str(key_file),
                "--binary-sha256", binary_sha, "--artifact-sha256", args.artifact_sha256,
                "--deployment-profile", "serve-resilience", "--max-context", "8192",
                "--kv-capacity", "8192", "--max-concurrency", "2", "--prefill-chunk", "1024",
                "--spec", "none", "--no-cuda-graph", "--no-thinking", "--host-context-mib", "8192",
                "--session-checkpoint-dir", str(checkpoints), "--session-checkpoint-require-origin-auth",
                "--session-checkpoint-min-tokens", "4294967295", "--session-checkpoint-quota-mib", "8192",
                "--shutdown-report", str(report), "--log-stats-interval-ms", "0"]
        argv += ["--config-sha256", hashlib.sha256(json.dumps(argv, separators=(",", ":")).encode()).hexdigest()]
        process = None
        log = None
        client = None
        def launch(epoch):
            nonlocal log, process
            log = (root / f"server-{epoch}.log").open("wb")
            process = ManagedProcess(argv, log)
            deadline = time.monotonic() + 180
            while time.monotonic() < deadline:
                require(process.child.poll() is None, "server exited during startup")
                try:
                    if request("GET", "/health")[0] == 200: return
                except (OSError, urllib.error.URLError): pass
                time.sleep(0.05)
            raise RuntimeError("server readiness deadline exceeded")
        try:
            launch(1)
            assert process is not None and log is not None
            chat = {"model": "serve-resilience", "messages": [{"role": "user", "content": "Say OK."}],
                    "max_tokens": 1, "temperature": 0, "reasoning_effort": "high"}
            code, _ = request("POST", "/v1/chat/completions", chat)
            require(code == 200, "high effort did not return HTTP 200")
            receipt["high_http_status"] = code
            code, _ = request("POST", "/v1/messages/count_tokens", {
                "model": "serve-resilience", "messages": [{"role": "user", "content": "hi"}],
                "thinking": {"type": "disabled"}, "output_config": {"effort": "max"}})
            require(code == 200, "disabled thinking plus effort did not return HTTP 200")
            receipt["disabled_effort_http_status"] = code
            history = dict(chat, reasoning_effort="none", messages=[
                {"role": "user", "content": "Use lookup."},
                {"role": "assistant", "content": "", "tool_calls": [{"id": "call_bad", "type": "function",
                    "function": {"name": "lookup", "arguments": "{\"text\":\"unfinished"}}]},
                {"role": "tool", "tool_call_id": "call_bad", "content": "Invalid input."},
                {"role": "user", "content": "Say OK instead."}])
            code, _ = request("POST", "/v1/chat/completions", history)
            require(code == 200, "malformed historical arguments poisoned the following turn")
            receipt["malformed_history_http_status"] = code
            code, seed = request("POST", "/v1/responses", {"model": "serve-resilience",
                "prompt_cache_key": cache_key, "input": "Name one primary color.", "temperature": 0,
                "max_output_tokens": 16, "store": True})
            require(code == 200 and isinstance(seed.get("id"), str), "durable seed failed")
            code, state = request("GET", "/v1/ninfer/checkpoints/status", session_header=True)
            require(code == 200 and state.get("state") == "missing", "seed was saved before Stop")
            client = http.client.HTTPConnection("127.0.0.1", port, timeout=120)
            client.request("POST", "/v1/chat/completions", json.dumps({"model": "serve-resilience",
                "stream": True, "temperature": 0, "max_tokens": 4096, "messages": [{"role": "user",
                    "content": "Write 2000 numbered detailed sentences about the solar system. Do not stop early."}]}),
                {"Authorization": "Bearer " + key, "Content-Type": "application/json"})
            response = client.getresponse()
            require(response.status == 200, "live stream did not start")
            code, metrics = request("GET", "/metrics", text=True)
            require(code == 200 and re.search(r"^ninfer_requests_running [1-9]", metrics, re.M) is not None,
                    "shutdown request was not resident")
            started = time.monotonic()
            require(process.stop(timeout=10) == 0, "connected-stream Stop failed")
            elapsed = time.monotonic() - started
            wire = response.read().decode()
            require("service_unavailable" in wire, "live stream did not receive a shutdown error")
            client.close(); client = None
            log.close(); log = None; process = None
            summary = json.loads(report.read_text())
            require(summary["complete"] is True and summary["saved"] >= 1 and summary["refused"] == 0,
                    "Stop did not acknowledge durable final saves")
            pointer = checkpoints / "sessions" / session / "current"
            require(pointer.is_file(), "Stop did not publish the durable session")
            receipt["stop_seconds"] = elapsed
            receipt["shutdown_report"] = summary
            launch(2)
            assert process is not None and log is not None
            code, restored = request("GET", "/v1/responses/" + seed["id"], session_header=True)
            require(code == 200 and restored.get("id") == seed["id"], "stored response did not restore")
            code, continuation = request("POST", "/v1/responses", {"model": "serve-resilience",
                "prompt_cache_key": cache_key, "previous_response_id": seed["id"],
                "input": "Name another one.", "max_output_tokens": 8, "temperature": 0, "store": True})
            cached = continuation.get("usage", {}).get("input_tokens_details", {}).get("cached_tokens", 0)
            require(code == 200 and cached > 0, "durable native continuation did not resume")
            receipt["restored_cached_tokens"] = cached
            require(process.stop(timeout=10) == 0, "restored server Stop failed")
            log.close(); log = None; process = None
            receipt["passed"] = True
        except BaseException:
            for path in root.glob("server-*.log"):
                print(path.read_text(errors="replace"))
            raise
        finally:
            if client is not None: client.close()
            if process is not None and process.child.poll() is None:
                process.child.kill(); process.child.wait(timeout=10)
            if log is not None: log.close()
    text = json.dumps(receipt, indent=2) + "\n"
    if args.receipt: args.receipt.write_text(text)
    print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
