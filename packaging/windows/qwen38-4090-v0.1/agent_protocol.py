#!/usr/bin/env python3
"""Exercise the authenticated agent protocol contract of a packaged NInfer server."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from typing import Any

from serve_contract import ContractError, json_response, request


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ContractError(message)


def digest(label: str) -> str:
    return hashlib.sha256(label.encode("utf-8")).hexdigest()


def read_key(path: Path) -> str:
    text = path.read_text(encoding="utf-8")
    value = text.rstrip("\r\n")
    require(bool(value) and "\n" not in value and "\r" not in value, "invalid API-key file")
    return value


def parse_sse(body: bytes) -> list[dict[str, Any]]:
    events: list[dict[str, Any]] = []
    for raw_line in body.decode("utf-8").splitlines():
        if not raw_line.startswith("data: "):
            continue
        payload = raw_line[6:]
        if payload == "[DONE]":
            continue
        value = json.loads(payload)
        require(isinstance(value, dict), "stream event is not an object")
        events.append(value)
    require(bool(events), "stream emitted no JSON events")
    return events


def chat_payload(model: str, prompt: str, session: str, request_id: str) -> dict[str, Any]:
    return {
        "model": model,
        "messages": [{"role": "user", "content": prompt}],
        "max_completion_tokens": 16,
        "temperature": 0,
        "stream": False,
        "ninfer_session": session,
        "ninfer_request_id": request_id,
    }


def run(args: argparse.Namespace) -> dict[str, Any]:
    api_key = read_key(args.api_key_file)
    headers = {"Authorization": f"Bearer {api_key}"}

    unauthenticated = json_response(
        args.base_url, "GET", "/v1/ninfer/status", expected_status=401
    )
    require("error" in unauthenticated, "unauthenticated status did not return an error")

    status = json_response(args.base_url, "GET", "/v1/ninfer/status", headers=headers)
    require(status.get("artifact_type") == "ninfer_server_status", "status artifact type mismatch")
    require(status.get("schema_version") == 1 and status.get("status") == "ok", "status envelope mismatch")
    for group in ("identity", "runtime", "scheduler", "cache", "mtp"):
        require(isinstance(status.get(group), dict), f"status is missing {group}")
    identity = status["identity"]
    expected_identity = {
        "binary_sha256": args.expect_binary_sha256,
        "model_artifact_sha256": args.expect_model_artifact_sha256,
        "config_sha256": args.expect_config_sha256,
        "deployment_profile": args.expect_deployment_profile,
    }
    for key, expected in expected_identity.items():
        require(identity.get(key) == expected, f"status identity mismatch: {key}")
    require(identity.get("source_dirty") is False, "status reports a dirty release build")

    malformed = chat_payload(args.model, "Say ready.", "not-a-sha", digest("malformed"))
    malformed_error = json_response(
        args.base_url,
        "POST",
        "/v1/chat/completions",
        malformed,
        headers=headers,
        expected_status=400,
    )
    require("error" in malformed_error, "malformed agent identity was accepted")

    tool_schema = {
        "type": "function",
        "function": {
            "name": "weather_lookup",
            "description": "Look up a forecast.",
            "parameters": {
                "type": "object",
                "properties": {
                    "city": {"type": "string"},
                    "days": {"type": "integer"},
                    "metric": {"type": "boolean"},
                },
                "required": ["city", "days", "metric"],
                "additionalProperties": False,
            },
        },
    }
    duplicate = chat_payload(args.model, "Use the tool.", digest("duplicate-session"), digest("duplicate-request"))
    duplicate["tools"] = [tool_schema, tool_schema]
    duplicate_error = json_response(
        args.base_url,
        "POST",
        "/v1/chat/completions",
        duplicate,
        headers=headers,
        expected_status=400,
    )
    require("error" in duplicate_error, "duplicate tool names were accepted")

    structured = chat_payload(args.model, "Return JSON.", digest("structured-session"), digest("structured-request"))
    structured["response_format"] = {
        "type": "json_schema",
        "json_schema": {
            "name": "answer",
            "strict": True,
            "schema": {"type": "object", "properties": {"answer": {"type": "string"}}},
        },
    }
    structured_error = json_response(
        args.base_url,
        "POST",
        "/v1/chat/completions",
        structured,
        headers=headers,
        expected_status=400,
    )
    require("error" in structured_error, "unsupported structured output was accepted")

    tool_request = chat_payload(
        args.model,
        "Call weather_lookup for Paris for exactly 3 days with metric true.",
        digest("tool-session"),
        digest("tool-request"),
    )
    tool_request["tools"] = [tool_schema]
    tool_request["tool_choice"] = {"type": "function", "function": {"name": "weather_lookup"}}
    tool_response = json_response(
        args.base_url, "POST", "/v1/chat/completions", tool_request, headers=headers
    )
    calls = tool_response["choices"][0]["message"].get("tool_calls")
    require(isinstance(calls, list) and len(calls) == 1, "forced tool call missing")
    arguments = json.loads(calls[0]["function"]["arguments"])
    require(isinstance(arguments.get("city"), str), "tool string argument lost its type")
    require(isinstance(arguments.get("days"), int), "tool integer argument lost its type")
    require(isinstance(arguments.get("metric"), bool), "tool boolean argument lost its type")

    tool_followup = {
        "model": args.model,
        "messages": [
            tool_request["messages"][0],
            tool_response["choices"][0]["message"],
            {
                "role": "tool",
                "tool_call_id": calls[0]["id"],
                "content": [{"type": "text", "text": "Paris: 18 C and clear."}],
            },
        ],
        "max_completion_tokens": 16,
        "temperature": 0,
        "ninfer_session": digest("tool-session"),
        "ninfer_request_id": digest("tool-followup"),
    }
    followup = json_response(
        args.base_url, "POST", "/v1/chat/completions", tool_followup, headers=headers
    )
    require(isinstance(followup["choices"][0]["message"].get("content"), str), "tool-result array failed")

    parity_prompt = "Reply with exactly: parity-ok"
    nonstream_payload = chat_payload(
        args.model, parity_prompt, digest("parity-session"), digest("parity-nonstream")
    )
    nonstream = json_response(
        args.base_url, "POST", "/v1/chat/completions", nonstream_payload, headers=headers
    )
    expected_text = nonstream["choices"][0]["message"]["content"]
    stream_payload = chat_payload(
        args.model, parity_prompt, digest("parity-session"), digest("parity-stream")
    )
    stream_payload["stream"] = True
    stream_payload["stream_options"] = {"include_usage": True}
    stream_response = request(
        args.base_url,
        "POST",
        "/v1/chat/completions",
        stream_payload,
        headers=headers,
    )
    require(stream_response.content_type == "text/event-stream", "stream content type mismatch")
    stream_events = parse_sse(stream_response.body)
    stream_text = "".join(
        choice.get("delta", {}).get("content", "")
        for event in stream_events
        for choice in event.get("choices", [])
        if isinstance(choice, dict)
    )
    require(stream_text == expected_text, "stream and nonstream content diverged")
    require(any(isinstance(event.get("usage"), dict) for event in stream_events), "stream usage missing")

    responses_session = digest("responses-session")
    create = json_response(
        args.base_url,
        "POST",
        "/v1/responses",
        {
            "model": args.model,
            "input": "Reply with one word: ready",
            "max_output_tokens": 16,
            "ninfer_session": responses_session,
            "ninfer_request_id": digest("responses-create"),
        },
        headers=headers,
    )
    response_id = create.get("id")
    require(isinstance(response_id, str) and response_id.startswith("resp_"), "Responses id missing")
    continued = json_response(
        args.base_url,
        "POST",
        "/v1/responses",
        {
            "model": args.model,
            "previous_response_id": response_id,
            "input": "Now reply with one word: done",
            "max_output_tokens": 16,
            "ninfer_session": responses_session,
            "ninfer_request_id": digest("responses-continue"),
        },
        headers=headers,
    )
    require(continued.get("previous_response_id") == response_id, "Responses continuation link missing")
    cross_session = json_response(
        args.base_url,
        "POST",
        "/v1/responses",
        {
            "model": args.model,
            "previous_response_id": response_id,
            "input": "This must not resolve.",
            "max_output_tokens": 8,
            "ninfer_session": digest("other-responses-session"),
            "ninfer_request_id": digest("responses-cross-session"),
        },
        headers=headers,
        expected_status=404,
    )
    require("error" in cross_session, "cross-session Responses DAG access was accepted")

    count = json_response(
        args.base_url,
        "POST",
        "/v1/messages/count_tokens",
        {"model": args.model, "messages": [{"role": "user", "content": "Count these tokens."}]},
        headers=headers,
    )
    require(isinstance(count.get("input_tokens"), int) and count["input_tokens"] > 0, "Anthropic count failed")

    return {
        "artifact_type": "ninfer_agent_protocol_smoke",
        "schema_version": 1,
        "status": "passed",
        "identity": expected_identity,
        "checks": {
            "authenticated_status": True,
            "malformed_identity_rejected": True,
            "duplicate_tool_names_rejected": True,
            "unsupported_structured_output_rejected": True,
            "typed_tool_arguments": True,
            "tool_result_array": True,
            "streaming_parity": True,
            "responses_continuation": True,
            "responses_cross_session_isolation": True,
            "anthropic_count_tokens": True,
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-url", required=True)
    parser.add_argument("--model", required=True)
    parser.add_argument("--api-key-file", required=True, type=Path)
    parser.add_argument("--expect-binary-sha256", required=True)
    parser.add_argument("--expect-model-artifact-sha256", required=True)
    parser.add_argument("--expect-config-sha256", required=True)
    parser.add_argument("--expect-deployment-profile", required=True)
    args = parser.parse_args()
    try:
        result = run(args)
    except (ContractError, KeyError, IndexError, json.JSONDecodeError, OSError) as error:
        print(json.dumps({"status": "failed", "error": str(error)}, sort_keys=True))
        return 1
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
