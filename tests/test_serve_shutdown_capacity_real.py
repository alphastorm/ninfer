#!/usr/bin/env python3
"""Calibrated real-model durable Stop pressure proof. Never run without an authorized GPU window."""
from __future__ import annotations

import argparse
from decimal import Decimal
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import subprocess
import time
import urllib.error
import urllib.request

from serve_real_process import ManagedProcess
from test_serve_restore_real import build_provenance, digest_file, proof_workdir, require, semantic_text

BASE = "0628db0b0997c8bda4ac37c8e84da95986890014"
METRICS = ("host_context_capacity_bytes", "host_context_used_bytes", "host_context_reserved_bytes",
           "host_state_images", "host_kv_used_bytes", "device_state_used_slots", "device_state_capacity_slots")


def read_metrics(text: str) -> dict[str, int]:
    values = {}
    for name in METRICS:
        match = re.search(rf"^ninfer_{name} ([0-9]+)(?:\.0+)?$", text, re.M)
        require(match is not None, "required physical occupancy metric is absent")
        values[name] = int(match.group(1))
    return values


def generation(root: Path, session: str) -> tuple[Path, dict]:
    current = root / "sessions" / session / "current"
    name = current.read_text(encoding="ascii").strip()
    require(re.fullmatch(r"[a-zA-Z0-9_-]+", name) is not None, "invalid current-pointer generation")
    folder = current.parent / "generations" / name
    manifest = json.loads((folder / "manifest.json").read_text(encoding="utf-8"))
    return folder, manifest


def payload_shape(root: Path, session: str) -> dict:
    folder, manifest = generation(root, session)
    files = manifest["files"]
    states = [f for f in files if f["path"].startswith("engine/state/")]
    kv = [f for f in files if f["path"].startswith("engine/kv/")]
    index = next(f for f in files if f["path"] == "engine/index.cbor")
    require(states and len({f["bytes"] for f in states}) == 1, "inconsistent native state geometry")
    for file in files:
        require((folder / file["path"]).stat().st_size == file["bytes"] and
                digest_file(folder / file["path"]) == file["sha256"], "published payload failed byte verification")
    return {"states": len(states), "state_bytes": sum(f["bytes"] for f in states),
            "state_stride": states[0]["bytes"], "state_digests": sorted(f["sha256"] for f in states),
            "kv_bytes": sum(f["bytes"] for f in kv), "index_bytes": index["bytes"],
            "native_bytes": sum(f["bytes"] for f in states + kv), "frontier": manifest["frontier_tokens"]}


def constrained_capacity(a: dict, b: dict, continuation: dict) -> int:
    minimum = max(a["native_bytes"], b["native_bytes"], continuation["native_bytes"])
    candidate = max(minimum, a["state_bytes"] + b["state_bytes"] // 2)
    capacity = (candidate + 255) // 256 * 256
    require(a["state_bytes"] < capacity < a["state_bytes"] + b["state_bytes"],
            "configured fixture cannot fit each full native set while creating a Host byte shortage")
    return capacity


def pressure_proof(metrics: dict, a: dict, b: dict, capacity: int) -> int:
    require(metrics["host_context_capacity_bytes"] == capacity and
            metrics["host_context_reserved_bytes"] == 0 and metrics["host_kv_used_bytes"] == 0 and
            metrics["host_context_used_bytes"] == a["state_bytes"] and
            metrics["host_state_images"] == a["states"] and
            metrics["device_state_used_slots"] == b["states"],
            "unexpected sharing, cached session, Host KV or demotion invalidates pressure fixture")
    demand = b["state_bytes"]
    free = capacity - metrics["host_context_used_bytes"]
    require(0 < demand - free <= a["state_bytes"] and demand <= capacity,
            "fixture did not prove a reclaimable byte deficit")
    return demand - free


def extract_classification(log: str, source: str, binary: str, delta: str) -> dict:
    require(re.fullmatch(r"[0-9a-f]{40}", delta or "") is not None and source == delta and
            re.fullmatch(r"[0-9a-f]{64}", binary) is not None,
            "classification binary must be the clean diagnostic commit")
    records = []
    for line in log.splitlines():
        try: record = json.loads(line)
        except ValueError: continue
        if isinstance(record, dict) and record.get("schema") == "ninfer-export-host-classification/1":
            records.append(record)
    require(len(records) == 1, "expected exactly one native Host refusal classification")
    record = records[0]
    require(record.get("base_sha") == BASE and
            record.get("native_site") == "StateImageStore::prepare_host_export" and
            record.get("kind") == "HostCapacity" and
            type(record.get("shortage_bytes")) is int and record["shortage_bytes"] > 0,
            "diagnostic did not classify the predecessor byte-shortage site")
    return {key: record[key] for key in ("base_sha", "native_site", "kind", "shortage_bytes")} | {
        "source_sha": source, "binary_sha256": binary, "diagnostic_delta_sha": delta}


def durable_receipts(log: str) -> list[dict]:
    values = []
    for line in log.splitlines():
        try: value = json.loads(line)
        except ValueError: continue
        if isinstance(value, dict) and value.get("schema") == "ninfer-durable-operation/1": values.append(value)
    return values


def receipt_summary(receipts: list[dict]) -> dict:
    counts: dict[str, int] = {}
    for value in receipts:
        key = "/".join(str(value.get(name)) for name in ("operation", "state", "reason"))
        counts[key] = counts.get(key, 0) + 1
    return {"counts": dict(sorted(counts.items())),
            "transaction_busy": sum(value.get("reason") == "transaction_busy" for value in receipts)}


def n89_verdict(expect: str, occupant: str, continuation: dict, stop: dict,
                pending: list[dict], stopping: list[dict]) -> None:
    """Fork #89 under reduced Host: B's fresh continuation, then a managed Stop.

    The continuation is served, or held behind B's pressure save when only releasing B's uncovered
    state frees its Device slot. That save, and B's Final save, need Host room the arena lacks on the
    predecessor; the candidate stages them from Device.
    """
    def save(value: dict, committed: bool) -> bool:
        return value.get("operation") == "save" and bool(value.get("committed")) == committed
    served = continuation.get("status") == 200 and continuation.get("text_equal") is True
    latched = continuation.get("status") == 503 and continuation.get("error_code") == "request_queue_timeout"
    staged = [v for v in pending + stopping if save(v, True) and v.get("device_staged_states", 0) > 0]
    if expect == "red":
        require(served or (latched and any(save(v, False) and v.get("reason") == "capacity" for v in pending)),
                "fresh continuation was neither served nor held behind a capacity-refused save")
        require(not staged, "predecessor staged a save from Device")
        if occupant == "deleted":
            require(stop["refused"] >= 1 and not stop["complete"] and
                    any(save(v, False) and v.get("reason") == "capacity" for v in stopping),
                    "Stop did not refuse the uncovered session for capacity")
        else:
            require(stop["refused"] == 0 and stop["complete"] and
                    any(save(v, True) and v.get("reclaimed_owners", 0) >= 1 for v in stopping),
                    "Stop did not reclaim the covered occupant")
        return
    require(served, "fresh continuation failed or differs from the Host-route reference")
    if occupant == "deleted":
        require(staged, "no save was Device-staged")
    if any(save(v, True) and v.get("device_staged_states", 0) > 0 for v in pending):
        require(continuation.get("state_digests_equal") is True,
                "Device-staged pressure save differs from the Host-route State digests")
    require(stop["refused"] == 0 and stop["complete"], "Stop refused a session")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--artifact", type=Path, default=os.environ.get("NINFER_TEST_ARTIFACT"))
    parser.add_argument("--artifact-sha256", default=os.environ.get("NINFER_TEST_ARTIFACT_SHA256"))
    parser.add_argument("--device-state-slots", type=int, default=os.environ.get("NINFER_TEST_DEVICE_STATE_SLOTS"))
    parser.add_argument("--spec", default=os.environ.get("NINFER_TEST_SPEC", "dflash2"), choices=("none", "mtp", "dflash", "dflash2"))
    parser.add_argument("--prompt-lines", type=int, default=600)
    parser.add_argument("--max-context", type=int, default=32768)
    parser.add_argument("--kv-capacity", type=int, default=32768)
    parser.add_argument("--roomy-host-mib", type=int, default=16384)
    parser.add_argument("--frontier-tolerance", type=int, default=64)
    parser.add_argument("--port", type=int, default=18132)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--expect-defect", action="store_true")
    mode.add_argument("--classify", action="store_true",
                      help="run instrumented predecessor and extract binary-bound native evidence")
    mode.add_argument("--n89", choices=("red", "green"),
                      help="fresh continuation of an uncovered Device-resident session under reduced Host")
    parser.add_argument("--diagnostic-delta-sha")
    parser.add_argument("--predecessor-classification", type=Path,
                        help="binary-bound classification-only diagnostic evidence for the predecessor")
    parser.add_argument("--receipt", type=Path)
    parser.add_argument("--occupant", choices=("deleted", "covered"), default="deleted",
                        help="--n89 Host occupant A: saved, then its response deleted; or saved and covered")
    parser.add_argument("--pending-timeout-ms", type=int, default=15000,
                        help="--n89 queue deadline of the pressure server")
    parser.add_argument("--evidence-dir", type=Path, help="--n89 copy of the server logs and Stop reports")
    args = parser.parse_args()
    defect_mode = args.expect_defect or args.classify
    if args.artifact is None:
        print("SKIP: explicit real-model artifact is required")
        return 77
    require(args.server.is_file() and args.artifact.is_file(), "explicit binary/artifact prerequisite missing")
    require(args.device_state_slots is not None and args.device_state_slots >= 0,
            "select extra Device state slots explicitly for this artifact/backend/GPU")
    require(args.prompt_lines > 0 and 0 <= args.frontier_tolerance <= 64, "invalid fixture geometry")
    require(args.pending_timeout_ms > 0, "pending timeout must be positive")
    artifact_sha, binary_sha = digest_file(args.artifact), digest_file(args.server)
    require(args.artifact_sha256 is None or artifact_sha == args.artifact_sha256, "artifact identity mismatch")
    version = subprocess.run([str(args.server), "--version"], check=True, capture_output=True, text=True).stdout.strip()
    source, upstream = build_provenance(version)
    classification = None
    if args.classify:
        require(args.predecessor_classification is not None and
                args.diagnostic_delta_sha == source,
                "classification output and exact diagnostic commit are required")
    if args.expect_defect:
        require(args.predecessor_classification is not None,
                "predecessor red requires native refusal-site evidence, not just reason=capacity")
        classification = json.loads(args.predecessor_classification.read_text(encoding="utf-8"))
        require(classification.get("base_sha") == BASE and classification.get("binary_sha256") == binary_sha and
                classification.get("source_sha") == source and
                classification.get("diagnostic_delta_sha") == source and
                classification.get("native_site") == "StateImageStore::prepare_host_export" and
                classification.get("kind") == "HostCapacity" and
                re.fullmatch(r"[0-9a-f]{40}", classification.get("diagnostic_delta_sha", "")) is not None,
                "missing binary-bound classification-only predecessor instrumentation evidence")
    receipt = {"artifact_type": "ninfer-serve-shutdown-capacity-proof/1", "source": source,
               "upstream_base_sha": upstream, "binary_sha256": binary_sha, "artifact_sha256": artifact_sha,
               "expect_defect": defect_mode, "classification_only": args.classify, "production_path": True,
               "fixture": {"extra_device_state_slots": args.device_state_slots, "spec": args.spec,
                           "prompt_lines": args.prompt_lines, "kv_capacity": args.kv_capacity}}
    if args.n89:
        receipt["artifact_type"] = "ninfer-serve-pending-capacity-proof/1"
        receipt["n89"] = {"expect": args.n89, "occupant": args.occupant,
                          "pending_timeout_ms": args.pending_timeout_ms}
    with proof_workdir() as root:
        key = secrets.token_hex(32)
        key_file = root / "api.key"
        key_file.write_text(key, encoding="ascii")
        key_file.chmod(0o600)
        cache_keys = [secrets.token_hex(16), secrets.token_hex(16)]
        sessions = [hashlib.sha256(b"ninfer:prompt_cache_key:v1\0" + value.encode()).hexdigest() for value in cache_keys]
        prompts = ["\n".join(f"{word} {i:04d}: {sentence}" for i in range(args.prompt_lines))
                   for word, sentence in (("Astronomy", "Chart the distant stars and their spectral classification."),
                                           ("Botany", "Record the local trees and their seasonal leaf structure."))]
        def request(method, route, body=None, capability=None, text=False):
            headers = {"Authorization": "Bearer " + key}
            if capability: headers["X-NInfer-Session"] = capability
            if body is not None: headers["Content-Type"] = "application/json"
            req = urllib.request.Request(f"http://127.0.0.1:{args.port}" + route,
                data=None if body is None else json.dumps(body).encode(), headers=headers, method=method)
            try:
                with urllib.request.urlopen(req, timeout=300) as response:
                    value = response.read().decode() if text else json.load(response)
                    return response.status, value
            except urllib.error.HTTPError as error:
                return error.code, json.load(error)
        class Server:
            def __init__(self, epoch, checkpoint_root, host_bytes, pending_timeout_ms=None):
                self.report = root / f"shutdown-{epoch}.json"
                self.log_path = root / f"server-{epoch}.log"
                self.log = self.log_path.open("wb")
                argv = [str(args.server), str(args.artifact), "--host", "127.0.0.1", "--port", str(args.port),
                    "--model-id", "shutdown-capacity-proof", "--api-key-file", str(key_file),
                    "--binary-sha256", binary_sha, "--artifact-sha256", artifact_sha,
                    "--deployment-profile", "shutdown-capacity-proof", "--max-context", str(args.max_context),
                    "--kv-capacity", str(args.kv_capacity), "--max-concurrency", "1", "--prefill-chunk", "1024",
                    "--device-state-slots", str(args.device_state_slots),
                    "--host-context-mib", format(Decimal(host_bytes) / Decimal(1048576), "f"),
                    "--kv-dtype", "bf16", "--spec", args.spec, "--session-checkpoint-dir", str(checkpoint_root),
                    "--session-checkpoint-require-origin-auth", "--session-checkpoint-min-tokens", "4294967295",
                    "--session-checkpoint-quota-mib", str(8 * args.roomy_host_mib),
                    "--shutdown-report", str(self.report), "--log-stats-interval-ms", "0", "--no-thinking"]
                if args.spec != "none": argv += ["--draft-tokens", "7", "--lm-head-draft"]
                if pending_timeout_ms is not None: argv += ["--pending-timeout-ms", str(pending_timeout_ms)]
                argv += ["--config-sha256", hashlib.sha256(json.dumps(argv, separators=(",", ":")).encode()).hexdigest()]
                self.process = ManagedProcess(argv, self.log)
                deadline = time.monotonic() + 300
                try:
                    while time.monotonic() < deadline:
                        require(self.process.child.poll() is None, "server exited before readiness")
                        try:
                            if request("GET", "/health")[0] == 200: return
                        except (OSError, urllib.error.URLError): pass
                        time.sleep(0.1)
                    raise RuntimeError("readiness deadline exceeded")
                except BaseException:
                    self.process.stop()
                    self.log.close()
                    raise
            def finish(self):
                try: code = self.process.stop(timeout=300)
                finally: self.log.close()
                require(self.report.is_file(), "synchronous shutdown report missing")
                report = json.loads(self.report.read_text(encoding="utf-8"))
                observed = {name: report[name] for name in ("saved", "skipped", "refused", "complete")}
                require((code == 0) == observed["complete"], "Stop exit status disagrees with its report")
                return observed
            def stop(self, expected):
                # expected: one exact report, or a tuple of acceptable exact reports.
                observed = self.finish()
                allowed = expected if isinstance(expected, tuple) else (expected,)
                require(observed in allowed, "unexpected Stop result or exit status")
                return observed
            def receipts(self):
                return durable_receipts(self.log_path.read_text(encoding="utf-8", errors="replace"))
        def seed(which):
            code, value = request("POST", "/v1/responses", {"model": "shutdown-capacity-proof",
                "prompt_cache_key": cache_keys[which], "input": prompts[which], "temperature": 0,
                "max_output_tokens": 16, "store": True})
            require(code == 200 and isinstance(value.get("id"), str) and semantic_text(value), "seed request failed")
            return value
        def save(which):
            code, value = request("POST", "/v1/ninfer/checkpoints", {"session_sha256": sessions[which]})
            require(code == 200 and value.get("committed") is True and value.get("reason") == "none", "explicit calibration save failed")
            return value
        active = None
        try:
            calibration = root / "calibration"
            active = Server(1, calibration, args.roomy_host_mib * 1048576)
            seed(0); save(0)
            a = payload_shape(calibration, sessions[0])
            b_seed = seed(1); save(1)
            b = payload_shape(calibration, sessions[1])
            require(not set(a["state_digests"]) & set(b["state_digests"]), "calibration state sets share contents")
            continuation = {"model": "shutdown-capacity-proof", "prompt_cache_key": cache_keys[1],
                "input": [{"role": "user", "content": prompts[1]},
                          {"role": "assistant", "content": semantic_text(b_seed)},
                          {"role": "user", "content": "Summarize one more observation in a short sentence."}],
                "temperature": 0, "max_output_tokens": 16, "store": True}
            code, reference = request("POST", "/v1/responses", continuation)
            require(code == 200 and semantic_text(reference), "reference continuation failed")
            save(1)
            next_b = payload_shape(calibration, sessions[1])
            active.stop({"saved": 0, "skipped": 2, "refused": 0, "complete": True}); active = None
            capacity = constrained_capacity(a, b, next_b)
            receipt["calibration"] = {name: {k: v for k, v in shape.items() if k != "state_digests"}
                                      for name, shape in (("a", a), ("b", b), ("continuation", next_b))}
            receipt["host_capacity_bytes"] = capacity
            checkpoints = root / "pressure"
            active = Server(2, checkpoints, capacity, args.pending_timeout_ms if args.n89 else None)
            a_seed = seed(0); save(0)
            require(payload_shape(checkpoints, sessions[0]) == a, "A calibration changed under constrained capacity")
            if args.n89 and args.occupant == "deleted":
                # Deleting A's only response erases its generation and records. A's named native state
                # stays Host-resident: no Automatic save may reclaim it and it is no provable Final victim.
                code, value = request("DELETE", "/v1/responses/" + a_seed["id"], capability=sessions[0])
                require(code == 200 and value.get("deleted") is True, "occupant response deletion failed")
                code, status = request("GET", "/v1/ninfer/checkpoints/status", capability=sessions[0])
                require(code == 200 and status.get("state") == "missing", "deleted occupant kept its generation")
            seeded = len(active.receipts())
            actual_b = seed(1)
            require(semantic_text(actual_b) == semantic_text(b_seed), "B calibration changed under constrained capacity")
            code, status = request("GET", "/v1/ninfer/checkpoints/status", capability=sessions[1])
            require(code == 200 and status.get("state") == "missing", "B was saved before Stop")
            code, text = request("GET", "/metrics", text=True)
            require(code == 200, "occupancy metrics unavailable")
            metrics = read_metrics(text)
            shortage = pressure_proof(metrics, a, b, capacity)
            receipt["before_stop"] = metrics
            receipt["host_shortage_bytes"] = shortage
            pointer = checkpoints / "sessions" / sessions[0] / "current"
            a_pointer = None if args.n89 else digest_file(pointer)
            if args.n89:
                # B's continuation forks B's newest image into a Device slot. When no slot is free and
                # none can take a demotion, only releasing one of B's images frees one, and B (named,
                # unsaved) must be saved first while its State images are Device-only. Then Stop saves
                # B's newest turn under the same Host shortage.
                shapes = {"seed": b, "continuation": next_b}
                def saved_b():
                    # B's current generation against the calibrated Host-route save of the same frontier.
                    if not (checkpoints / "sessions" / sessions[1] / "current").is_file(): return None
                    shape = payload_shape(checkpoints, sessions[1])
                    match = next((name for name, value in shapes.items() if value["frontier"] == shape["frontier"]), None)
                    return {"generation": match, "frontier": shape["frontier"], "state_digests": shape["state_digests"],
                            "state_digests_equal": match is not None and
                                                   shape["state_digests"] == shapes[match]["state_digests"]}
                started = time.monotonic()
                code, value = request("POST", "/v1/responses", continuation)
                outcome = {"status": code, "seconds": round(time.monotonic() - started, 3)}
                if code == 200:
                    text = semantic_text(value)
                    staged = saved_b()
                    outcome |= {"text_equal": text == semantic_text(reference),
                                "sha256": hashlib.sha256(text.encode()).hexdigest(), "saved": staged,
                                "state_digests_equal": staged is not None and staged["state_digests_equal"]}
                else:
                    outcome["error_code"] = (value.get("error") or {}).get("code")
                receipt["continuation"] = outcome
                pending = active.receipts()[seeded:]
                code, text = request("GET", "/metrics", text=True)
                require(code == 200, "occupancy metrics unavailable")
                receipt["after_continuation"] = read_metrics(text)
                server, active = active, None
                receipt["stop"] = server.finish()
                stopping = server.receipts()[seeded + len(pending):]
                summaries = {"pending": receipt_summary(pending), "stop": receipt_summary(stopping)}
                receipt["receipts"] = {"pending": pending, "stop": stopping, "summary": summaries,
                                       "transaction_busy": sum(s["transaction_busy"] for s in summaries.values())}
                receipt["final_generation"] = saved_b()
                n89_verdict(args.n89, args.occupant, outcome, receipt["stop"], pending, stopping)
            elif defect_mode:
                receipt["stop"] = active.stop({"saved": 0, "skipped": 1, "refused": 1, "complete": False})
                if args.classify:
                    classification = extract_classification(
                        active.log_path.read_text(encoding="utf-8", errors="replace"),
                        source, binary_sha, args.diagnostic_delta_sha)
                require(classification.get("shortage_bytes") == shortage, "native diagnostic deficit differs from measured fixture")
                receipt["predecessor_classification"] = {key: classification[key] for key in (
                    "base_sha", "source_sha", "binary_sha256", "diagnostic_delta_sha", "native_site",
                    "kind", "shortage_bytes")}
                require(any(v.get("operation") == "save" and v.get("reason") == "capacity" and
                            not v.get("committed") for v in active.receipts()), "predecessor did not refuse native Capacity")
                if args.classify:
                    args.predecessor_classification.write_text(
                        json.dumps(classification, sort_keys=True, indent=2) + "\n", encoding="utf-8")
                active = None
            else:
                receipt["stop"] = active.stop({"saved": 1, "skipped": 1, "refused": 0, "complete": True})
                saves = [v for v in active.receipts() if v.get("operation") == "save" and v.get("committed") and
                         v.get("reclaimed_owners", 0) >= 1 and v.get("reclaimed_host_bytes", 0) > 0]
                require(len(saves) == 1, "Stop succeeded without proving durable-victim Host reclaim")
                receipt["reclaim"] = saves[0]
                active = None
                require(payload_shape(checkpoints, sessions[1])["native_bytes"] <= capacity,
                        "saved B does not fit an empty arena")
                active = Server(3, checkpoints, capacity)
                code, restored = request("POST", "/v1/responses", continuation)
                require(code == 200 and semantic_text(restored) == semantic_text(reference), "restart continuation differs from reference")
                cached = restored.get("usage", {}).get("input_tokens_details", {}).get("cached_tokens", 0)
                require(cached >= b["frontier"] - args.frontier_tolerance, "B did not restore the saved native frontier")
                require(digest_file(pointer) == a_pointer and payload_shape(checkpoints, sessions[0]) == a,
                        "reclaiming save changed or quota-evicted A")
                receipt["restart"] = {"cached_tokens": cached, "continuation_sha256": hashlib.sha256(
                    semantic_text(restored).encode()).hexdigest(), "a_current_unchanged": True}
                # B's restored continuation fills the calibrated arena, so importing A beside it is
                # a Capacity refusal (first 5090 run). Completing the continuation can also queue a
                # pressure save that publishes B before Stop (later 5090 runs); otherwise Stop
                # saves B. Either way B's current generation must be the continuation.
                receipt["second_stop"] = active.stop((
                    {"saved": 0, "skipped": 1, "refused": 0, "complete": True},
                    {"saved": 1, "skipped": 0, "refused": 0, "complete": True}))
                active = None
                durable = payload_shape(checkpoints, sessions[1])
                shape = ("states", "state_bytes", "kv_bytes", "native_bytes", "frontier")
                require({k: durable[k] for k in shape} == {k: next_b[k] for k in shape},
                        "B's restored continuation is not durable after the second Stop")
                require(digest_file(pointer) == a_pointer, "second Stop changed A's covered generation")
                active = Server(4, checkpoints, capacity)
                code, _ = request("GET", "/v1/responses/" + a_seed["id"], capability=sessions[0])
                require(code == 200, "cold victim A is not loadable after restart")
                receipt["victim_load_stop"] = active.stop({"saved": 0, "skipped": 1, "refused": 0, "complete": True})
                active = None
                require(digest_file(pointer) == a_pointer, "loading A changed its covered generation")
            receipt["passed"] = True
        except BaseException as error:
            if args.n89:
                receipt["passed"] = False
                receipt["failure"] = f"{type(error).__name__}: {error}"
            raise
        finally:
            if active is not None:
                try: active.process.stop(timeout=300)
                finally: active.log.close()
            if args.n89 and args.evidence_dir:
                # The reproduction keeps every receipt: server logs and Stop reports, pass or fail.
                args.evidence_dir.mkdir(parents=True, exist_ok=True)
                for path in sorted(root.glob("server-*.log")) + sorted(root.glob("shutdown-*.json")):
                    shutil.copy2(path, args.evidence_dir / path.name)
            if args.n89 and args.receipt and not receipt.get("passed"):
                args.receipt.write_text(json.dumps(receipt, sort_keys=True, indent=2) + "\n", encoding="utf-8")
    encoded = json.dumps(receipt, sort_keys=True, indent=2) + "\n"
    if args.receipt: args.receipt.write_text(encoded, encoding="utf-8")
    print(encoded, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
