#!/usr/bin/env python3
"""Exercise NInfer's authenticated agent-protocol and private-session contract."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys
import re
from typing import Any, Mapping

from serve_contract import (
    ContractError,
    json_response,
    parse_openai_stream,
    request,
    wait_for_health,
)


_SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
_PRIVATE_REUSE_PATHS = {
    "private_endpoint",
    "private_turn_closure",
    "private_response_replay",
    "private_long_anchor",
}


class ProtocolError(RuntimeError):
    pass


def digest(label: str) -> str:
    return hashlib.sha256(f"ninfer-agent-protocol-v1/{label}".encode()).hexdigest()


def read_api_key(path: Path | None) -> str | None:
    if path is None:
        return None
    try:
        value = path.read_bytes().rstrip(b"\r\n")
    except OSError as error:
        raise ProtocolError("failed to read --api-key-file") from error
    if not value or b"\n" in value or b"\r" in value or b"\x00" in value:
        raise ProtocolError("--api-key-file must contain one non-empty line")
    try:
        return value.decode("utf-8")
    except UnicodeDecodeError as error:
        raise ProtocolError("--api-key-file must contain UTF-8") from error


def auth_headers(api_key: str | None) -> dict[str, str]:
    return {"Authorization": f"Bearer {api_key}"} if api_key is not None else {}


def error_code(value: dict[str, Any]) -> str | None:
    error = value.get("error")
    if not isinstance(error, dict):
        return None
    code = error.get("code")
    return code if isinstance(code, str) else None


def expect_error(
    base_url: str,
    path: str,
    payload: dict[str, Any],
    *,
    headers: Mapping[str, str],
    status: int,
    code: str,
) -> None:
    value = json_response(
        base_url,
        "POST",
        path,
        payload,
        headers=headers,
        expected_status=status,
    )
    if error_code(value) != code:
        raise ProtocolError(f"{path} returned the wrong error code")


def validate_status_identity(status: dict[str, Any], args: argparse.Namespace) -> None:
    if (
        status.get("artifact_type") != "ninfer_server_status"
        or status.get("schema_version") != 1
        or status.get("status") != "ok"
    ):
        raise ProtocolError("status endpoint returned the wrong envelope")
    identity = status.get("identity")
    runtime = status.get("runtime")
    if (
        not isinstance(identity, dict)
        or not isinstance(runtime, dict)
        or not isinstance(status.get("scheduler"), dict)
        or not isinstance(status.get("cache"), dict)
        or not isinstance(status.get("mtp"), dict)
    ):
        raise ProtocolError("status endpoint omitted a versioned contract group")
    expected = {
        "binary_sha256": args.expect_binary_sha256,
        "model_artifact_sha256": args.expect_model_artifact_sha256,
        "config_sha256": args.expect_config_sha256,
        "deployment_profile": args.expect_deployment_profile,
    }
    for field, value in expected.items():
        if value is not None and identity.get(field) != value:
            raise ProtocolError(
                f"status identity field {field} differs from its expected value"
            )
    if runtime.get("public_model_id") != args.model:
        raise ProtocolError("status public_model_id differs from --model")


def cache_reuse_path(status: dict[str, Any]) -> str:
    cache = status.get("cache")
    selection = cache.get("last_selection") if isinstance(cache, dict) else None
    path = selection.get("path") if isinstance(selection, dict) else None
    if not isinstance(path, str):
        raise ProtocolError("status omitted cache.last_selection.path")
    return path


def parse_choice(response: dict[str, Any]) -> dict[str, Any]:
    choices = response.get("choices")
    if (
        not isinstance(choices, list)
        or len(choices) != 1
        or not isinstance(choices[0], dict)
    ):
        raise ProtocolError("Chat Completions response has the wrong choices shape")
    return choices[0]


def require_usage(response: dict[str, Any]) -> tuple[int, int]:
    usage = response.get("usage")
    if not isinstance(usage, dict):
        raise ProtocolError("Chat Completions response omitted usage")
    prompt = usage.get("prompt_tokens")
    completion = usage.get("completion_tokens")
    if (
        not isinstance(prompt, int)
        or prompt <= 0
        or not isinstance(completion, int)
        or completion < 0
    ):
        raise ProtocolError("Chat Completions response has invalid usage")
    return prompt, completion


def identity_fields(session: str, request_label: str) -> dict[str, str]:
    return {
        "ninfer_session": session,
        "ninfer_request_id": digest(f"request/{request_label}"),
    }


def authenticated_identity_checks(
    base_url: str,
    model: str,
    headers: Mapping[str, str],
    session_a: str,
) -> int:
    malformed = {
        "model": model,
        "messages": [{"role": "user", "content": "Reply briefly."}],
        "max_completion_tokens": 1,
        "ninfer_session": "A" * 64,
    }
    expect_error(
        base_url,
        "/v1/chat/completions",
        malformed,
        headers=headers,
        status=400,
        code="invalid_ninfer_identity",
    )

    counted = json_response(
        base_url,
        "POST",
        "/v1/messages/count_tokens",
        {
            "model": model,
            "max_tokens": 4,
            "messages": [{"role": "user", "content": "Reply briefly."}],
            **identity_fields(session_a, "anthropic-count"),
        },
        headers=headers,
    )
    input_tokens = counted.get("input_tokens")
    if not isinstance(input_tokens, int) or input_tokens <= 0:
        raise ProtocolError(
            "authenticated Anthropic count_tokens returned invalid usage"
        )
    return input_tokens




def structured_output_rejection(
    base_url: str, model: str, headers: Mapping[str, str], identity: dict[str, str]
) -> None:
    expect_error(
        base_url,
        "/v1/chat/completions",
        {
            "model": model,
            "messages": [{"role": "user", "content": "Return JSON."}],
            "max_completion_tokens": 1,
            "response_format": {
                "type": "json_schema",
                "json_schema": {
                    "name": "answer",
                    "schema": {"type": "object", "properties": {}},
                },
            },
            **identity,
        },
        headers=headers,
        status=400,
        code="response_format_not_supported",
    )


def tool_round_trip(
    base_url: str,
    model: str,
    headers: Mapping[str, str],
    session: str | None,
) -> str:
    tool = {
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
            },
        },
    }
    payload: dict[str, Any] = {
        "model": model,
        "messages": [
            {
                "role": "user",
                "content": "Call lookup_weather for Paris for exactly 2 days.",
            }
        ],
        "tools": [tool],
        "tool_choice": {"type": "function", "function": {"name": "lookup_weather"}},
        "max_completion_tokens": 96,
        "temperature": 0,
    }
    if session is not None:
        payload.update(identity_fields(session, "tool-call"))
    response = json_response(
        base_url, "POST", "/v1/chat/completions", payload, headers=headers
    )
    choice = parse_choice(response)
    message = choice.get("message")
    calls = message.get("tool_calls") if isinstance(message, dict) else None
    if not isinstance(calls, list) or len(calls) != 1 or not isinstance(calls[0], dict):
        raise ProtocolError("forced tool request did not return exactly one tool call")
    function = calls[0].get("function")
    if not isinstance(function, dict) or function.get("name") != "lookup_weather":
        raise ProtocolError("forced tool request returned the wrong function")
    raw_arguments = function.get("arguments")
    if not isinstance(raw_arguments, str):
        raise ProtocolError("tool arguments are not a JSON string")
    try:
        arguments = json.loads(raw_arguments)
    except json.JSONDecodeError as error:
        raise ProtocolError("tool arguments are not valid JSON") from error
    if not isinstance(arguments, dict) or not isinstance(arguments.get("city"), str):
        raise ProtocolError("tool string argument has the wrong type")
    if not isinstance(arguments.get("days"), int) or isinstance(
        arguments.get("days"), bool
    ):
        raise ProtocolError("tool integer argument has the wrong type")

    assistant = {
        key: value
        for key, value in message.items()
        if key in {"role", "content", "reasoning_content", "tool_calls"}
    }
    continuation: dict[str, Any] = {
        "model": model,
        "messages": [
            payload["messages"][0],
            assistant,
            {
                "role": "tool",
                "tool_call_id": calls[0].get("id"),
                "content": [{"type": "text", "text": "Paris: clear, 21 C."}],
            },
        ],
        "tools": [tool],
        "max_completion_tokens": 16,
        "temperature": 0,
    }
    if session is not None:
        continuation.update(identity_fields(session, "tool-result-array"))
    completed = json_response(
        base_url, "POST", "/v1/chat/completions", continuation, headers=headers
    )
    finish_reason = parse_choice(completed).get("finish_reason")
    if finish_reason not in {"stop", "length", "tool_calls"}:
        raise ProtocolError(
            "tool-result continuation returned an invalid finish_reason"
        )
    return str(finish_reason)


def streaming_and_session_checks(
    base_url: str,
    model: str,
    headers: Mapping[str, str],
    status_headers: Mapping[str, str],
    session_a: str | None,
    session_b: str | None,
) -> tuple[str, str, int, str | None, str | None]:
    messages = [{"role": "user", "content": "Reply with one short word."}]
    common: dict[str, Any] = {
        "model": model,
        "messages": messages,
        "max_completion_tokens": 8,
        "temperature": 0,
    }
    first_payload = dict(common)
    if session_a is not None:
        first_payload.update(identity_fields(session_a, "stream-base"))
    first = json_response(
        base_url, "POST", "/v1/chat/completions", first_payload, headers=headers
    )
    first_choice = parse_choice(first)
    first_message = first_choice.get("message")
    if not isinstance(first_message, dict):
        raise ProtocolError("non-streaming response omitted its message")
    expected_content = first_message.get("content") or ""
    expected_reasoning = first_message.get("reasoning_content") or ""
    if not isinstance(expected_content, str) or not isinstance(expected_reasoning, str):
        raise ProtocolError("non-streaming response content has the wrong type")
    _, completion_tokens = require_usage(first)

    stream_payload = {
        **common,
        "stream": True,
        "stream_options": {"include_usage": True},
    }
    if session_a is not None:
        stream_payload.update(identity_fields(session_a, "stream-repeat"))
    streamed = request(
        base_url,
        "POST",
        "/v1/chat/completions",
        stream_payload,
        headers=headers,
    )
    content, reasoning, finish_reason, usage = parse_openai_stream(streamed)
    if (content, reasoning) != (expected_content, expected_reasoning):
        raise ProtocolError("streaming output differs from greedy non-streaming output")
    if finish_reason != first_choice.get("finish_reason"):
        raise ProtocolError("streaming finish_reason differs from non-streaming output")
    if usage != first.get("usage"):
        raise ProtocolError("streaming usage differs from non-streaming output")

    private_path: str | None = None
    isolated_path: str | None = None
    if session_a is not None and session_b is not None:
        status_after_repeat = json_response(
            base_url, "GET", "/v1/ninfer/status", headers=status_headers
        )
        private_path = cache_reuse_path(status_after_repeat)
        if private_path not in _PRIVATE_REUSE_PATHS:
            raise ProtocolError(
                "same-session repeat did not select a private continuation"
            )

        isolated_payload = dict(common)
        isolated_payload.update(identity_fields(session_b, "isolated-session"))
        isolated = json_response(
            base_url, "POST", "/v1/chat/completions", isolated_payload, headers=headers
        )
        require_usage(isolated)
        status_after_isolated = json_response(
            base_url, "GET", "/v1/ninfer/status", headers=status_headers
        )
        isolated_path = cache_reuse_path(status_after_isolated)
        if isolated_path in _PRIVATE_REUSE_PATHS:
            raise ProtocolError(
                "a new session selected another session's private continuation"
            )

    return (
        str(finish_reason),
        str(first_choice.get("finish_reason")),
        completion_tokens,
        private_path,
        isolated_path,
    )


def response_id(value: dict[str, Any]) -> str:
    identifier = value.get("id")
    if not isinstance(identifier, str) or not identifier.startswith("resp_"):
        raise ProtocolError("Responses object omitted its response id")
    return identifier


def responses_session_lifecycle(
    base_url: str,
    model: str,
    headers: Mapping[str, str],
    session_a: str,
    session_b: str,
) -> dict[str, Any]:
    def create(label: str, text: str, previous: str | None = None) -> dict[str, Any]:
        payload: dict[str, Any] = {
            "model": model,
            "input": text,
            "max_output_tokens": 16,
            "temperature": 0,
            "store": True,
            **identity_fields(session_a, f"responses/{label}"),
        }
        if previous is not None:
            payload["previous_response_id"] = previous
        return json_response(
            base_url, "POST", "/v1/responses", payload, headers=headers
        )

    first = create("first", "Reply with one short word.")
    first_id = response_id(first)
    continuation = create("continuation", "Reply with another short word.", first_id)
    continuation_id = response_id(continuation)
    branch = create("branch", "Reply with a different short word.", first_id)
    branch_id = response_id(branch)

    cross_tenant = {
        "model": model,
        "input": "This must be rejected.",
        "previous_response_id": first_id,
        "max_output_tokens": 16,
        **identity_fields(session_b, "responses/cross-tenant"),
    }
    rejected = json_response(
        base_url,
        "POST",
        "/v1/responses",
        cross_tenant,
        headers=headers,
        expected_status=404,
    )
    if error_code(rejected) != "response_not_found":
        raise ProtocolError("cross-session previous_response_id was not isolated")

    deleted = json_response(
        base_url, "DELETE", f"/v1/responses/{first_id}", headers=headers
    )
    if deleted.get("deleted") is not True or deleted.get("id") != first_id:
        raise ProtocolError("Responses parent deletion returned the wrong object")
    missing = json_response(
        base_url,
        "GET",
        f"/v1/responses/{first_id}",
        headers=headers,
        expected_status=404,
    )
    if error_code(missing) != "response_not_found":
        raise ProtocolError("deleted Responses parent remained addressable")

    surviving = create(
        "post-delete-continuation",
        "Reply with one final short word.",
        continuation_id,
    )
    surviving_id = response_id(surviving)
    branch_read = json_response(
        base_url, "GET", f"/v1/responses/{branch_id}", headers=headers
    )
    if response_id(branch_read) != branch_id:
        raise ProtocolError("Responses fork disappeared after parent deletion")
    return {
        "first_turn_completed": True,
        "continuation_completed": True,
        "fork_count": 2,
        "cross_session_status": 404,
        "parent_delete_status": 200,
        "parent_get_after_delete_status": 404,
        "surviving_descendants_retrieved": True,
        "surviving_descendant_continued": True,
    }


def exercise(args: argparse.Namespace) -> dict[str, Any]:
    base_url = args.base_url.rstrip("/")
    api_key = read_api_key(args.api_key_file)
    headers = auth_headers(api_key)
    wait_for_health(base_url, args.health_timeout)

    unauthorized = json_response(
        base_url,
        "GET",
        "/v1/ninfer/status",
        expected_status=401,
    )
    if error_code(unauthorized) != "invalid_api_key":
        raise ProtocolError("status route did not reject a missing API key")

    status = json_response(base_url, "GET", "/v1/ninfer/status", headers=headers)
    validate_status_identity(status, args)
    session_a = digest("session/a")
    session_b = digest("session/b")
    responses = responses_session_lifecycle(
        base_url, args.model, headers, session_a, session_b
    )

    # The same digests drive Chat/Anthropic parity checks after the Responses DAG exercise.
    anthropic_tokens = authenticated_identity_checks(
        base_url, args.model, headers, session_a
    )
    active_session_a = session_a
    active_session_b = session_b
    structured_identity = identity_fields(session_a, "structured-rejection")

    structured_output_rejection(base_url, args.model, headers, structured_identity)
    tool_finish = tool_round_trip(base_url, args.model, headers, active_session_a)
    stream_finish, nonstream_finish, completion_tokens, private_path, isolated_path = (
        streaming_and_session_checks(
            base_url,
            args.model,
            headers,
            headers,
            active_session_a,
            active_session_b,
        )
    )
    return {
        "artifact_type": "ninfer_agent_protocol_smoke",
        "schema_version": 1,
        "status_identity_verified": True,
        "authenticated_session_identity": True,
        "anthropic_count_tokens": anthropic_tokens,
        "structured_output_rejected": True,
        "tool_result_array_finish_reason": tool_finish,
        "stream_finish_reason": stream_finish,
        "nonstream_finish_reason": nonstream_finish,
        "completion_tokens": completion_tokens,
        "same_session_reuse_path": private_path,
        "isolated_session_reuse_path": isolated_path,
        "responses_session_lifecycle": responses,
    }


def sha256_argument(value: str) -> str:
    if not _SHA256_RE.fullmatch(value):
        raise argparse.ArgumentTypeError("expected 64 lowercase hexadecimal characters")
    return value


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-url", default="http://127.0.0.1:18080")
    parser.add_argument("--model", required=True)
    parser.add_argument("--api-key-file", type=Path, required=True)
    parser.add_argument("--health-timeout", type=float, default=300.0)
    parser.add_argument("--expect-binary-sha256", type=sha256_argument)
    parser.add_argument("--expect-model-artifact-sha256", type=sha256_argument)
    parser.add_argument("--expect-config-sha256", type=sha256_argument)
    parser.add_argument("--expect-deployment-profile")
    args = parser.parse_args()
    try:
        print(json.dumps(exercise(args), sort_keys=True))
    except (ContractError, ProtocolError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
