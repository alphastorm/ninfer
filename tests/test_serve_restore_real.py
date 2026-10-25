#!/usr/bin/env python3
"""Production HTTP/disk/Engine restart proof; emits counts/hashes, never model text or capabilities."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import secrets
import signal
import subprocess
import tempfile
import time
import urllib.error
import urllib.request


def digest_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(8 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def semantic_text(response: dict) -> str:
    return "".join(part.get("text", "") for item in response.get("output", [])
                   for part in item.get("content", []) if part.get("type") == "output_text")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--artifact", type=Path, default=os.environ.get("NINFER_TEST_ARTIFACT"))
    parser.add_argument("--artifact-sha256", default=os.environ.get("NINFER_TEST_ARTIFACT_SHA256"))
    parser.add_argument("--port", type=int, default=18130)
    parser.add_argument("--receipt", type=Path)
    args = parser.parse_args()
    if args.artifact is None:
        print("SKIP: explicit real-model artifact is required")
        return 77
    require(args.server.is_file() and args.artifact.is_file(), "explicit server/artifact prerequisite missing")
    artifact_sha = args.artifact_sha256 or digest_file(args.artifact)
    binary_sha = digest_file(args.server)
    receipt: dict = {"artifact_type": "ninfer-serve-restore-proof/1", "binary_sha256": binary_sha,
                     "artifact_sha256": artifact_sha, "production_path": True, "tracer": False}
    with tempfile.TemporaryDirectory(prefix="serve-restore-") as scratch:
        root = Path(scratch)
        checkpoint_root = root / "checkpoints"
        api_key = secrets.token_hex(32)
        key_file = root / "api.key"
        key_file.write_text(api_key, encoding="ascii")
        key_file.chmod(0o600)
        cache_key = secrets.token_hex(16)
        session = hashlib.sha256(b"ninfer:prompt_cache_key:v1\0" + cache_key.encode()).hexdigest()
        foreign = hashlib.sha256(b"foreign-test-capability" + cache_key.encode()).hexdigest()
        base_url = f"http://127.0.0.1:{args.port}"
        config_sha = hashlib.sha256(b"serve-restore-real:bf16:dflash2:k7:c1:32768:v1").hexdigest()

        def request(method: str, route: str, body: dict | None = None,
                    capability: str | None = None) -> tuple[int, dict]:
            headers = {"Authorization": f"Bearer {api_key}"}
            if capability is not None:
                headers["X-NInfer-Session"] = capability
            encoded = None if body is None else json.dumps(body).encode()
            if encoded is not None:
                headers["Content-Type"] = "application/json"
            req = urllib.request.Request(base_url + route, data=encoded, headers=headers, method=method)
            try:
                with urllib.request.urlopen(req, timeout=180) as response:
                    return response.status, json.load(response)
            except urllib.error.HTTPError as error:
                return error.code, json.load(error)

        class Server:
            def __init__(self, epoch: int, extra: tuple[str, ...] = ()) -> None:
                self.report = root / f"shutdown-{epoch}.json"
                self.log = (root / f"server-{epoch}.log").open("wb")
                argv = [str(args.server), str(args.artifact), "--host", "127.0.0.1", "--port", str(args.port),
                        "--model-id", "serve-restore-proof", "--api-key-file", str(key_file),
                        "--binary-sha256", binary_sha, "--artifact-sha256", artifact_sha,
                        "--config-sha256", config_sha, "--deployment-profile", "serve-restore-proof",
                        "--max-context", "32768", "--kv-capacity", "32768", "--max-concurrency", "1",
                        "--prefill-chunk", "1024", "--device-state-slots", "4", "--host-context-mib", "8192",
                        "--kv-dtype", "bf16", "--spec", "dflash2", "--draft-tokens", "7", "--lm-head-draft",
                        "--session-checkpoint-dir", str(checkpoint_root), "--session-checkpoint-require-origin-auth",
                        "--session-checkpoint-min-tokens", "4294967295", "--shutdown-report", str(self.report),
                        "--log-stats-interval-ms", "0", "--no-thinking", *extra]
                self.child = subprocess.Popen(argv, stdout=self.log, stderr=subprocess.STDOUT)
                deadline = time.monotonic() + 180
                try:
                    while time.monotonic() < deadline:
                        require(self.child.poll() is None, "production server exited before readiness")
                        try:
                            code, _ = request("GET", "/health")
                            if code == 200:
                                return
                        except (OSError, urllib.error.URLError):
                            pass
                        time.sleep(0.1)
                    raise RuntimeError("production readiness deadline exceeded")
                except BaseException:
                    self.terminate()
                    raise

            def terminate(self) -> dict:
                if self.child.poll() is None:
                    self.child.send_signal(signal.SIGTERM)
                try:
                    code = self.child.wait(timeout=180)
                except subprocess.TimeoutExpired:
                    self.child.kill()
                    self.child.wait()
                    self.log.close()
                    raise RuntimeError("managed final-save deadline exceeded") from None
                self.log.close()
                require(code == 0, "production stop did not acknowledge final saves")
                require(self.report.is_file(), "synchronous shutdown report missing")
                report = json.loads(self.report.read_text(encoding="utf-8"))
                require(report.get("artifact_type") == "ninfer_serve_shutdown_report" and
                        report.get("complete") is True and report.get("refused") == 0,
                        "shutdown reported incomplete/unacknowledged save")
                return report

        active: Server | None = None
        try:
            active = Server(1)
            prompt = "\n".join(f"Ledger {i:04d}: durable replay preserves exact committed state and ordered records."
                                for i in range(1100))
            seed_body = {"model": "serve-restore-proof", "prompt_cache_key": cache_key,
                         "input": prompt, "max_output_tokens": 16, "temperature": 0, "store": True}
            code, seed = request("POST", "/v1/responses", seed_body)
            require(code == 200 and isinstance(seed.get("id"), str), "seed Responses request failed")
            seed_text = semantic_text(seed)
            require(bool(seed_text), "seed produced no text")
            full_history = [{"role": "user", "content": prompt},
                            {"role": "assistant", "content": seed_text},
                            {"role": "user", "content": "Continue the ledger summary in one short sentence."}]
            continuation = {"model": "serve-restore-proof", "prompt_cache_key": cache_key,
                            "input": full_history, "max_output_tokens": 16, "temperature": 0, "store": False}
            code, reference = request("POST", "/v1/responses", continuation)
            require(code == 200, "reference continuation failed")
            reference_text = semantic_text(reference)
            require(bool(reference_text), "reference produced no text")
            # Nothing explicit or automatic has saved this below-minimum session.
            code, before_stop = request("GET", "/v1/ninfer/checkpoints/status", capability=session)
            require(code == 200 and before_stop.get("state") == "missing", "pre-stop session unexpectedly checkpointed")
            stopped = active.terminate()
            active = None
            require(stopped.get("saved", 0) >= 1, "short-before-auto-min session was not saved synchronously")
            receipt["first_stop"] = {key: stopped[key] for key in ("saved", "skipped", "refused", "complete")}
            current = checkpoint_root / "sessions" / session / "current"
            require(current.is_file(), "acknowledged generation has no current pointer")
            prior_pointer_sha = digest_file(current)

            # First keyed request has no previous_response_id and no retrieve before it.
            active = Server(2)
            continuation["store"] = True
            code, restored = request("POST", "/v1/responses", continuation)
            require(code == 200, "first keyed request could not restore production state")
            restored_text = semantic_text(restored)
            cached = restored.get("usage", {}).get("input_tokens_details", {}).get("cached_tokens", 0)
            require(cached >= 16384, "production restored continuation did not hit at least 16K tokens")
            require(restored_text == reference_text, "restart continuation differs from the live reference")
            receipt["restart"] = {"cached_input_tokens": cached,
                "continuation_bytes": len(restored_text.encode()),
                "continuation_sha256": hashlib.sha256(restored_text.encode()).hexdigest(),
                "byte_identical": True, "first_keyed_request_without_parent": True}
            for capability in (foreign, None):
                code, _ = request("GET", "/v1/responses/" + seed["id"], capability=capability)
                require(code == 404, "scoped retrieve leaked a private restored response")
                code, _ = request("DELETE", "/v1/responses/" + seed["id"], capability=capability)
                require(code == 404, "scoped delete accepted a foreign/missing capability")
            code, _ = request("GET", "/v1/responses/" + seed["id"], capability=session)
            require(code == 200, "own capability could not retrieve restored lineage")
            code, saved = request("POST", "/v1/ninfer/checkpoints", {"session_sha256": session})
            require(code == 200 and saved.get("operation") == "save" and saved.get("state") == "exported" and
                    saved.get("committed") is True and saved.get("reason") == "none" and
                    saved.get("origin") == "authenticated" and saved.get("dir_sync") == "fsync",
                    "explicit save did not acknowledge all production durability gates")
            receipt["save"] = {key: saved[key] for key in ("state", "reason", "committed", "origin", "dir_sync",
                "frontier_tokens", "restored_tokens", "payload_bytes", "points", "states", "pages")}
            active.terminate()
            active = None
            disabled_pointer_sha = digest_file(current)

            active = Server(3, ("--no-prefix-reuse",))
            code, _ = request("GET", "/v1/responses/" + seed["id"], capability=session)
            require(code == 404, "cache-disabled request imported native state")
            require(digest_file(current) == disabled_pointer_sha, "policy refusal mutated the disk generation")
            active.terminate()
            active = None
            receipt["policy_disabled"] = {"retrieve_status": 404, "current_unchanged": True}
            receipt["isolation"] = {"foreign_retrieve": 404, "foreign_delete": 404,
                                    "missing_capability_retrieve": 404, "own_retrieve": 200}
            receipt["prior_generation_published"] = bool(prior_pointer_sha)
            receipt["passed"] = True
        finally:
            if active is not None:
                active.terminate()
    if args.receipt is not None:
        args.receipt.parent.mkdir(parents=True, exist_ok=True)
        args.receipt.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(receipt, sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, OSError, ValueError) as error:
        # Deliberately do not print raw HTTP bodies, model text, argv, logs or host paths.
        print("FAIL: " + (str(error) if isinstance(error, RuntimeError) else type(error).__name__))
        raise SystemExit(1) from None
