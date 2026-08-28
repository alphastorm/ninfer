#!/usr/bin/env python3
"""Run one exact Chat Completions profiling request against an existing NInfer server."""

from __future__ import annotations

import argparse
import hashlib
import http.client
import json
import sys
import time
from pathlib import Path
from typing import Any, Sequence


REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.bench import run_serve_corpus as corpus  # noqa: E402


REQUEST_TIMEOUT_SECONDS = 24.0 * 60.0 * 60.0
LOG_EVENT_TIMEOUT_SECONDS = 10.0


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--model", default="q38-ninfer")
    parser.add_argument("--messages", type=Path, required=True)
    parser.add_argument("--max-new", type=int, required=True)
    parser.add_argument("--seed", type=int)
    parser.add_argument("--thinking", action="store_true")
    parser.add_argument("--request-log", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--expected-content")
    parser.add_argument("--timeout", type=float, default=REQUEST_TIMEOUT_SECONDS)
    parser.add_argument("--log-timeout", type=float, default=LOG_EVENT_TIMEOUT_SECONDS)
    args = parser.parse_args(argv)
    if args.port < 1 or args.port > 65535:
        parser.error("--port must be in [1, 65535]")
    if args.max_new <= 0:
        parser.error("--max-new must be positive")
    if args.timeout <= 0.0 or args.log_timeout <= 0.0:
        parser.error("timeouts must be positive")
    return args


def load_events(path: Path, offset: int = 0) -> list[dict[str, Any]]:
    try:
        with path.open("rb") as handle:
            handle.seek(offset)
            data = handle.read()
    except OSError as exc:
        raise corpus.CampaignError(f"failed to read request log {path}: {exc}") from exc
    if not data:
        return []
    if not data.endswith(b"\n"):
        data = data.rsplit(b"\n", 1)[0] + b"\n" if b"\n" in data else b""
    events: list[dict[str, Any]] = []
    for line in data.splitlines():
        if not line:
            continue
        try:
            event = json.loads(line)
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise corpus.CampaignError(f"request log contains invalid JSON: {exc}") from exc
        if not isinstance(event, dict):
            raise corpus.CampaignError("request log event is not a JSON object")
        events.append(event)
    return events


def current_server_instance(path: Path) -> str:
    starts = [event for event in load_events(path) if event.get("event") == "server_start"]
    if not starts:
        raise corpus.CampaignError("request log has no server_start event")
    event = starts[-1]
    corpus.require_server_log_identity(event, "server_start")
    server_instance_id = event.get("server_instance_id")
    if not isinstance(server_instance_id, str) or not server_instance_id:
        raise corpus.CampaignError("server_start has no server_instance_id")
    return server_instance_id


def wait_for_request_event(
    path: Path, offset: int, server_instance_id: str, timeout: float
) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        for event in load_events(path, offset):
            if event.get("server_instance_id") != server_instance_id:
                continue
            if event.get("event") == "request_error":
                raise corpus.CampaignError(f"serving request failed: {event!r}")
            if event.get("event") == "request_done":
                corpus.require_server_log_identity(event, "request_done")
                return event
        time.sleep(0.05)
    raise corpus.CampaignError("request_done event was not observed")


def require_object(value: Any, description: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise corpus.CampaignError(f"{description} is not a JSON object")
    return value


def run(args: argparse.Namespace) -> dict[str, Any]:
    try:
        messages = json.loads(args.messages.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise corpus.CampaignError(f"failed to load messages from {args.messages}: {exc}") from exc
    if not isinstance(messages, list):
        raise corpus.CampaignError("messages fixture is not a JSON array")

    payload: dict[str, Any] = {
        "model": args.model,
        "messages": messages,
        "max_completion_tokens": args.max_new,
        "stream": False,
        "enable_thinking": args.thinking,
    }
    if args.seed is not None:
        payload["seed"] = args.seed

    server_instance_id = current_server_instance(args.request_log)
    initial_offset = args.request_log.stat().st_size
    connection = http.client.HTTPConnection(args.host, args.port, timeout=args.timeout)
    started = time.monotonic()
    try:
        response = corpus.post_json(connection, payload)
    finally:
        connection.close()
    wall_seconds = time.monotonic() - started
    server_event = wait_for_request_event(
        args.request_log, initial_offset, server_instance_id, args.log_timeout
    )

    choices = response.get("choices")
    if not isinstance(choices, list) or len(choices) != 1:
        raise corpus.CampaignError("response does not contain exactly one choice")
    choice = require_object(choices[0], "response choice")
    message = require_object(choice.get("message"), "response message")
    content_value = message.get("content")
    content = "" if content_value is None else str(content_value)
    exact_match = args.expected_content is None or content.strip() == args.expected_content

    result = require_object(server_event.get("result"), "request_done result")
    timings = require_object(server_event.get("timings_seconds"), "request_done timings")
    speculative = require_object(server_event.get("speculative"), "request_done speculative metrics")
    try:
        prompt_tokens = int(result["prompt_tokens"])
        completion_tokens = int(result["completion_tokens"])
        prefill_seconds = float(timings["prefill"])
        decode_seconds = float(timings["decode"])
        drafted_tokens = int(speculative["drafted_tokens"])
        accepted_tokens = int(speculative["accepted_tokens"])
    except (KeyError, TypeError, ValueError) as exc:
        raise corpus.CampaignError(f"request_done is missing required metrics: {exc}") from exc

    usage = require_object(response.get("usage"), "response usage")
    if usage.get("prompt_tokens") != prompt_tokens or usage.get("completion_tokens") != completion_tokens:
        raise corpus.CampaignError("HTTP response usage does not match request_done token counts")

    decode_tokens = max(0, completion_tokens - 1)
    message_bytes = json.dumps(
        message, ensure_ascii=False, sort_keys=True, separators=(",", ":")
    ).encode("utf-8")
    summary = {
        "status": 200,
        "wall_seconds": wall_seconds,
        "prompt_tokens": prompt_tokens,
        "completion_tokens": completion_tokens,
        "finish_reason": result.get("finish_reason"),
        "prepare_seconds": float(timings["prepare"]),
        "vision_seconds": float(timings["vision"]),
        "prefill_seconds": prefill_seconds,
        "decode_seconds": decode_seconds,
        "total_seconds": float(timings["total"]),
        "prefill_tokens_per_second": corpus.safe_ratio(float(prompt_tokens), prefill_seconds),
        "decode_tokens_per_second": corpus.safe_ratio(float(decode_tokens), decode_seconds),
        "speculative_rounds": int(speculative["rounds"]),
        "drafted_tokens": drafted_tokens,
        "accepted_tokens": accepted_tokens,
        "speculative_acceptance": corpus.safe_ratio(float(accepted_tokens), float(drafted_tokens)),
        "content_sha256": hashlib.sha256(content.encode("utf-8")).hexdigest(),
        "message_sha256": hashlib.sha256(message_bytes).hexdigest(),
        "exact_match": exact_match,
    }
    record = {
        "summary": summary,
        "request": payload,
        "response": response,
        "server_event": server_event,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(record, ensure_ascii=False, sort_keys=True, indent=2) + "\n",
        encoding="utf-8",
    )
    if not exact_match:
        raise corpus.CampaignError("response content did not match the exact oracle")
    return summary


def main(argv: Sequence[str] | None = None) -> int:
    try:
        summary = run(parse_args(argv))
    except corpus.CampaignError as exc:
        print(f"run_profile_request: {exc}", file=sys.stderr)
        return 1
    print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
