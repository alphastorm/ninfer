#!/usr/bin/env python3
"""Exercise the authenticated agent protocol contract of a packaged NInfer server."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from typing import Any, Mapping

from serve_contract import ContractError, json_response, request


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ContractError(message)


def digest(label: str) -> str:
    return hashlib.sha256(label.encode("utf-8")).hexdigest()


def error_code(value: dict[str, Any]) -> str | None:
    error = value.get("error")
    if not isinstance(error, dict):
        return None
    code = error.get("code")
    return code if isinstance(code, str) else None


def stored_response_headers(headers: Mapping[str, str], session: str | None) -> dict[str, str]:
    scoped = dict(headers)
    if session is not None:
        scoped["X-NInfer-Session"] = session
    return scoped


def metric_counter(base_url: str, headers: Mapping[str, str], name: str) -> int:
    if name != "ninfer:prefix_cache_hit_tokens_total":
        raise ContractError(f"unsupported compatibility metric {name}")
    status = json_response(base_url, "GET", "/v1/ninfer/status", headers=headers)
    cache = status.get("cache")
    value = cache.get("reused_prompt_tokens") if isinstance(cache, dict) else None
    if not isinstance(value, int):
        raise ContractError("status omitted cache.reused_prompt_tokens")
    return value


def response_id(value: dict[str, Any]) -> str:
    identifier = value.get("id")
    require(
        isinstance(identifier, str) and identifier.startswith("resp_"),
        "Responses id missing",
    )
    return identifier


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
            "name": "lookup_weather",
            "description": "Look up concise weather data for a city.",
            "parameters": {
                "type": "object",
                "properties": {
                    "city": {"type": "string"},
                    "days": {"type": "integer"},
                },
                "required": ["city", "days"],
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
        "Call lookup_weather for Paris for exactly 2 days.",
        digest("tool-session"),
        digest("tool-request"),
    )
    tool_request["max_completion_tokens"] = 96
    tool_request["tools"] = [tool_schema]
    tool_request["tool_choice"] = {"type": "function", "function": {"name": "lookup_weather"}}
    tool_response = json_response(
        args.base_url, "POST", "/v1/chat/completions", tool_request, headers=headers
    )
    calls = tool_response["choices"][0]["message"].get("tool_calls")
    require(isinstance(calls, list) and len(calls) == 1, "forced tool call missing")
    arguments = json.loads(calls[0]["function"]["arguments"])
    require(isinstance(arguments.get("city"), str), "tool string argument lost its type")
    require(isinstance(arguments.get("days"), int), "tool integer argument lost its type")

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
    cache_hits_before_repeat = metric_counter(
        args.base_url, headers, "ninfer:prefix_cache_hit_tokens_total"
    )
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
    cache_hits_after_repeat = metric_counter(
        args.base_url, headers, "ninfer:prefix_cache_hit_tokens_total"
    )
    require(
        cache_hits_after_repeat > cache_hits_before_repeat,
        "same-session repeat did not use its private cache",
    )
    isolated_payload = chat_payload(
        args.model, parity_prompt, digest("isolated-session"), digest("isolated-request")
    )
    json_response(
        args.base_url, "POST", "/v1/chat/completions", isolated_payload, headers=headers
    )
    cache_hits_after_isolated = metric_counter(
        args.base_url, headers, "ninfer:prefix_cache_hit_tokens_total"
    )
    require(
        cache_hits_after_isolated == cache_hits_after_repeat,
        "a different session reused private cached tokens",
    )

    responses_session = digest("responses-session")
    other_responses_session = digest("other-responses-session")

    def create_response(
        label: str,
        text: str,
        previous: str | None = None,
        *,
        expected_status: int = 200,
    ) -> dict[str, Any]:
        payload: dict[str, Any] = {
            "model": args.model,
            "input": text,
            "max_output_tokens": 16,
            "temperature": 0,
            "store": True,
            "ninfer_session": responses_session,
            "ninfer_request_id": digest(f"responses-{label}"),
        }
        if previous is not None:
            payload["previous_response_id"] = previous
        return json_response(
            args.base_url,
            "POST",
            "/v1/responses",
            payload,
            headers=headers,
            expected_status=expected_status,
        )

    first = create_response("first", "Reply with one word: ready")
    first_id = response_id(first)
    continued = create_response("continue", "Now reply with one word: done", first_id)
    continued_id = response_id(continued)
    require(continued.get("previous_response_id") == first_id, "Responses continuation link missing")
    branch = create_response("branch", "Reply with a different one-word answer", first_id)
    branch_id = response_id(branch)

    conflict = json_response(
        args.base_url,
        "POST",
        "/v1/responses",
        {
            "model": args.model,
            "input": "This identity conflict must fail.",
            "max_output_tokens": 16,
            "ninfer_session": responses_session,
            "ninfer_request_id": digest("responses-identity-conflict"),
        },
        headers=stored_response_headers(headers, other_responses_session),
        expected_status=400,
    )
    require(error_code(conflict) == "invalid_ninfer_identity", "Responses identity conflict used the wrong error")

    cross_session = json_response(
        args.base_url,
        "POST",
        "/v1/responses",
        {
            "model": args.model,
            "previous_response_id": first_id,
            "input": "This must not resolve.",
            "max_output_tokens": 16,
            "ninfer_session": other_responses_session,
            "ninfer_request_id": digest("responses-cross-session"),
        },
        headers=headers,
        expected_status=404,
    )
    require(
        error_code(cross_session) == "previous_response_not_found",
        "cross-session previous_response_id used the wrong error",
    )

    def expect_stored_not_found(method: str, path: str, session: str | None, label: str) -> None:
        value = json_response(
            args.base_url,
            method,
            path,
            headers=stored_response_headers(headers, session),
            expected_status=404,
        )
        require(error_code(value) == "response_not_found", f"{label} was not isolated")

    for route, suffix, method in (
        ("retrieve", "", "GET"),
        ("input-items", "/input_items", "GET"),
        ("cancel", "/cancel", "POST"),
    ):
        path = f"/v1/responses/{first_id}{suffix}"
        expect_stored_not_found(method, path, None, f"session omission on Responses {route}")
        expect_stored_not_found(
            method, path, other_responses_session, f"cross-session Responses {route}"
        )

    scoped_headers = stored_response_headers(headers, responses_session)
    scoped_read = json_response(
        args.base_url, "GET", f"/v1/responses/{first_id}", headers=scoped_headers
    )
    require(response_id(scoped_read) == first_id, "same-session Responses retrieve returned the wrong object")
    scoped_items = json_response(
        args.base_url,
        "GET",
        f"/v1/responses/{first_id}/input_items",
        headers=scoped_headers,
    )
    require(scoped_items.get("object") == "list", "same-session input-items returned the wrong object")
    scoped_cancel = json_response(
        args.base_url,
        "POST",
        f"/v1/responses/{first_id}/cancel",
        headers=scoped_headers,
        expected_status=400,
    )
    require(
        error_code(scoped_cancel) == "background_not_supported",
        "same-session cancel did not reach the retained response",
    )

    streaming_payload = {
        "model": args.model,
        "previous_response_id": continued_id,
        "input": "Reply with one word: streamed",
        "max_output_tokens": 64,
        "temperature": 0,
        "reasoning": {"effort": "none"},
        "store": True,
        "stream": True,
        "ninfer_session": responses_session,
        "ninfer_request_id": digest("responses-stream"),
    }
    responses_stream = request(
        args.base_url,
        "POST",
        "/v1/responses",
        streaming_payload,
        headers=headers,
    )
    require(responses_stream.content_type == "text/event-stream", "Responses stream content type mismatch")
    responses_events = parse_sse(responses_stream.body)
    terminal_responses = [
        event.get("response")
        for event in responses_events
        if event.get("type") == "response.completed" and isinstance(event.get("response"), dict)
    ]
    require(len(terminal_responses) == 1, "Responses stream omitted its completed response")
    streamed = terminal_responses[0]
    streamed_id = response_id(streamed)
    require(streamed.get("previous_response_id") == continued_id, "streamed continuation link missing")
    streamed_read = json_response(
        args.base_url, "GET", f"/v1/responses/{streamed_id}", headers=scoped_headers
    )
    require(response_id(streamed_read) == streamed_id, "streamed response was not retained")

    expect_stored_not_found(
        "DELETE", f"/v1/responses/{first_id}", None, "session omission on Responses delete"
    )
    expect_stored_not_found(
        "DELETE",
        f"/v1/responses/{first_id}",
        other_responses_session,
        "cross-session Responses delete",
    )
    deleted = json_response(
        args.base_url, "DELETE", f"/v1/responses/{first_id}", headers=scoped_headers
    )
    require(
        deleted.get("deleted") is True and deleted.get("id") == first_id,
        "Responses parent deletion returned the wrong object",
    )
    expect_stored_not_found(
        "GET", f"/v1/responses/{first_id}", responses_session, "deleted Responses parent"
    )
    deleted_parent = create_response(
        "deleted-parent",
        "This continuation must be rejected.",
        first_id,
        expected_status=404,
    )
    require(
        error_code(deleted_parent) == "previous_response_not_found",
        "deleted parent used the wrong continuation error",
    )
    surviving = create_response(
        "post-delete-continuation", "Reply with one final short word", continued_id
    )
    surviving_id = response_id(surviving)
    require(bool(surviving_id), "surviving descendant could not continue")
    branch_read = json_response(
        args.base_url, "GET", f"/v1/responses/{branch_id}", headers=scoped_headers
    )
    require(response_id(branch_read) == branch_id, "Responses branch disappeared after parent deletion")

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
            "private_cache_session_isolation": True,
            "responses_continuation": True,
            "responses_streaming_checkpoint": True,
            "responses_cross_session_isolation": True,
            "responses_stored_route_isolation": True,
            "responses_parent_deletion": True,
            "responses_descendant_survival": True,
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
