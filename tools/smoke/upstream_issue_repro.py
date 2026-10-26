#!/usr/bin/env python3
"""Bounded real-server regressions for upstream #414 and #420 (no model mocks)."""
from __future__ import annotations

import argparse
import concurrent.futures
import json
import threading
import urllib.error
import urllib.request


def post(url: str, payload: dict, timeout: float = 90):
    request = urllib.request.Request(
        url + "/v1/chat/completions", json.dumps(payload).encode(),
        {"Content-Type": "application/json"}, method="POST")
    try:
        return urllib.request.urlopen(request, timeout=timeout)
    except urllib.error.HTTPError as error:
        return error


def payload(model: str, *, stream: bool, length: int, salt: int = 0) -> dict:
    return {"model": model, "stream": stream, "enable_thinking": False,
            "max_tokens": 32768 if length > 256 else 256,
            "temperature": 0, "messages": [{"role": "user", "content":
            f"Return a JSON string of lowercase letters. Request {salt}."}],
            "response_format": {"type": "json_schema", "json_schema": {
                "name": "long_string", "strict": True,
                "schema": {"type": "string", "minLength": length, "maxLength": length}}}}


def health(url: str) -> int:
    with urllib.request.urlopen(url + "/health", timeout=10) as response:
        return response.status


def prefix(url: str, model: str, concurrency: int) -> dict:
    counts = []
    for round_number in range(4):
        barrier = threading.Barrier(concurrency)
        def run(_):
            barrier.wait(timeout=10)
            with post(url, payload(model, stream=False, length=256, salt=round_number)) as response:
                body = json.load(response)
                assert response.status == 200 and "error" not in body, (response.status, body)
                assert body["choices"][0]["finish_reason"] == "stop", body
                content = json.loads(body["choices"][0]["message"]["content"])
                assert isinstance(content, str) and len(content) == 256
                return body["usage"]["completion_tokens"]
        with concurrent.futures.ThreadPoolExecutor(max_workers=concurrency) as pool:
            counts.extend(pool.map(run, range(concurrency)))
        assert health(url) == 200
    return {"issue": 414, "requests": len(counts), "completion_tokens": counts,
            "health": 200, "verdict": "not_reproduced"}


def timeout(url: str, model: str, concurrency: int) -> dict:
    blockers = []
    try:
        # Read a real content chunk from every resident, not merely HTTP headers.
        # minLength keeps them active while the short request waits for admission.
        for row in range(concurrency):
            response = post(url, payload(model, stream=True, length=8192, salt=100 + row))
            blockers.append(response)
            assert response.status == 200, (response.status, response.read().decode())
            while line := response.readline():
                if not line.startswith(b"data: "):
                    continue
                event = json.loads(line[6:])
                assert "error" not in event, event
                if any(choice.get("delta", {}).get("content") for choice in event.get("choices", [])):
                    break
            else:
                raise AssertionError("blocker ended before producing content")
        with post(url, {"model": model, "stream": True, "max_tokens": 1,
                       "messages": [{"role": "user", "content": "hi"}]}) as response:
            status = response.status
            body = response.read().decode()
            if status == 503:
                code = json.loads(body).get("error", {}).get("code")
            else:
                code = "request_queue_timeout" if "request_queue_timeout" in body else None
            result = {"issue": 420, "status": status, "code": code,
                      "content_type": response.headers.get("Content-Type"),
                      "blockers": concurrency, "verdict": "fixed" if status == 503 else "reproduced"}
            print(json.dumps(result), flush=True)
            assert status == 503 and code == "request_queue_timeout", result
            return result
    finally:
        for response in blockers:
            response.close()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("issue", choices=("414", "420"))
    parser.add_argument("--url", required=True)
    parser.add_argument("--model", required=True)
    parser.add_argument("--concurrency", type=int, default=2)
    args = parser.parse_args()
    if args.concurrency < 1:
        parser.error("--concurrency must match the server's positive max-concurrency")
    result = (prefix if args.issue == "414" else timeout)(args.url, args.model, args.concurrency)
    if args.issue == "414":
        print(json.dumps(result), flush=True)


if __name__ == "__main__":
    main()
