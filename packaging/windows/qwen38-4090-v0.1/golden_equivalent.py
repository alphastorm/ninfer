#!/usr/bin/env python3
"""Run the source-controlled RTX 4090 Golden-equivalent through OMP."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time
from typing import Any, Mapping, Sequence
from urllib.error import HTTPError, URLError
from urllib.parse import urlsplit
from urllib.request import Request, urlopen


CONTRACT_ARTIFACT = "ninfer_omp_golden_equivalent_contract"
RECEIPT_ARTIFACT = "ninfer_omp_golden_equivalent_receipt"
PROVIDER = "ninfer-golden-equivalent"
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
SOURCE_RE = re.compile(r"^[0-9a-f]{40}$")


class QualificationError(RuntimeError):
    """A deterministic qualification contract was not satisfied."""


def canonical_bytes(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode("utf-8")


def digest_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise QualificationError(message)


def exact_arguments(value: object, expected: Mapping[str, object]) -> bool:
    if not isinstance(value, dict) or set(value) != set(expected):
        return False
    return (
        type(value.get("city")) is str
        and value["city"] == expected["city"]
        and type(value.get("days")) is int
        and value["days"] == expected["days"]
        and type(value.get("metric")) is bool
        and value["metric"] == expected["metric"]
    )


def load_contract(path: Path) -> tuple[dict[str, Any], str, str]:
    raw = path.read_bytes()
    value = json.loads(raw.decode("utf-8"))
    require(isinstance(value, dict), "Golden-equivalent contract is not an object")
    require(value.get("artifact_type") == CONTRACT_ARTIFACT, "Golden-equivalent artifact type mismatch")
    require(value.get("schema_version") == 1, "Golden-equivalent schema version mismatch")
    require(value.get("historical_private_corpus_reused") is False, "private-corpus provenance must fail closed")
    require(
        value.get("fixture_provenance")
        == "source-controlled synthetic fixture; the unavailable historical private corpus was not reused",
        "Golden-equivalent fixture provenance mismatch",
    )
    tool = value.get("tool")
    require(isinstance(tool, dict), "Golden-equivalent tool contract is missing")
    expected = tool.get("expected_arguments")
    require(
        isinstance(expected, dict)
        and exact_arguments(expected, {"city": "Paris", "days": 3, "metric": True}),
        "Golden-equivalent expected arguments changed",
    )
    for field in ("contract_id", "system_prompt", "user_prompt", "final_visible_answer"):
        require(isinstance(value.get(field), str) and bool(value[field]), f"Golden-equivalent {field} is missing")
    for field in ("name", "label", "description", "result"):
        require(isinstance(tool.get(field), str) and bool(tool[field]), f"Golden-equivalent tool.{field} is missing")
    return value, digest_bytes(raw), digest_bytes(canonical_bytes(value))


def read_key(path: Path) -> str:
    raw = path.read_bytes()
    while raw.endswith((b"\r", b"\n")):
        raw = raw[:-1]
    require(bool(raw), "API-key file is empty")
    require(b"\r" not in raw and b"\n" not in raw and b"\0" not in raw, "API-key file must contain one line")
    try:
        return raw.decode("utf-8", errors="strict")
    except UnicodeDecodeError as error:
        raise QualificationError("API-key file is not UTF-8") from error


def endpoint_urls(base_url: str) -> tuple[str, str]:
    value = base_url.rstrip("/")
    parsed = urlsplit(value)
    require(parsed.scheme in ("http", "https") and bool(parsed.hostname), "base URL must be HTTP(S)")
    require(parsed.username is None and parsed.password is None, "base URL must not contain credentials")
    require(not parsed.query and not parsed.fragment, "base URL must not contain a query or fragment")
    path = parsed.path.rstrip("/")
    require(path in ("", "/v1"), "base URL path must be empty or /v1")
    if path == "/v1":
        return value, f"{value}/ninfer/status"
    return f"{value}/v1", f"{value}/v1/ninfer/status"


def request_json(url: str, key: str) -> dict[str, Any]:
    request = Request(url, headers={"Authorization": f"Bearer {key}"})
    try:
        with urlopen(request, timeout=30) as response:
            payload = response.read()
    except HTTPError as error:
        raise QualificationError(f"status endpoint returned HTTP {error.code}") from error
    except URLError as error:
        raise QualificationError(f"status endpoint failed: {error.reason}") from error
    value = json.loads(payload.decode("utf-8"))
    require(isinstance(value, dict), "status endpoint did not return an object")
    return value


def validate_status(
    status: Mapping[str, object],
    *,
    source_commit: str,
    binary_sha256: str,
    model_artifact_sha256: str,
    config_sha256: str,
    deployment_profile: str,
) -> dict[str, object]:
    require(status.get("artifact_type") == "ninfer_server_status", "status artifact type mismatch")
    require(status.get("schema_version") == 1 and status.get("status") == "ok", "status envelope mismatch")
    identity = status.get("identity")
    require(isinstance(identity, dict), "status identity is missing")
    expected = {
        "patch_stack_sha": source_commit,
        "binary_sha256": binary_sha256,
        "model_artifact_sha256": model_artifact_sha256,
        "config_sha256": config_sha256,
        "deployment_profile": deployment_profile,
    }
    for field, expected_value in expected.items():
        require(identity.get(field) == expected_value, f"status identity mismatch: {field}")
    require(identity.get("source_dirty") is False, "status reports a dirty source build")
    require(identity.get("cuda_architecture") == "89", "status is not the RTX 4090 sm_89 build")
    return {
        "upstream_base_sha": identity.get("upstream_base_sha"),
        "patch_stack_sha": source_commit,
        "source_dirty": False,
        "build_profile": identity.get("build_profile"),
        "build_type": identity.get("build_type"),
        "cuda_architecture": identity.get("cuda_architecture"),
        "deployment_profile": deployment_profile,
        "binary_sha256": binary_sha256,
        "model_artifact_sha256": model_artifact_sha256,
        "config_sha256": config_sha256,
    }


def visible_text(message: Mapping[str, object]) -> str:
    content = message.get("content")
    require(isinstance(content, list), "assistant content is not an array")
    texts: list[str] = []
    for block in content:
        if isinstance(block, dict) and block.get("type") == "text":
            text = block.get("text")
            require(isinstance(text, str), "assistant text block is malformed")
            texts.append(text)
    return "".join(texts)


def validate_transcript(events: Sequence[Mapping[str, object]], contract: Mapping[str, Any]) -> dict[str, object]:
    ended: list[tuple[int, Mapping[str, object]]] = []
    for index, event in enumerate(events):
        if event.get("type") != "message_end":
            continue
        message = event.get("message")
        if isinstance(message, dict):
            ended.append((index, message))

    assistants = [(index, message) for index, message in ended if message.get("role") == "assistant"]
    results = [(index, message) for index, message in ended if message.get("role") == "toolResult"]
    require(len(assistants) == 2, "OMP transcript must contain exactly two completed assistant messages")
    require(len(results) == 1, "OMP transcript must contain exactly one completed tool result")

    first_index, first = assistants[0]
    result_index, result = results[0]
    final_index, final = assistants[1]
    require(first_index < result_index < final_index, "OMP tool-result continuation order is invalid")

    first_content = first.get("content")
    require(isinstance(first_content, list), "typed-tool assistant content is not an array")
    calls = [block for block in first_content if isinstance(block, dict) and block.get("type") == "toolCall"]
    require(len(calls) == 1, "OMP did not emit exactly one typed tool call")
    require(visible_text(first) == "", "typed-tool assistant emitted visible text before the tool result")
    call = calls[0]
    tool = contract["tool"]
    require(call.get("name") == tool["name"], "OMP invoked the wrong tool")
    require(exact_arguments(call.get("arguments"), tool["expected_arguments"]), "typed argument oracle rejected OMP output")
    call_id = call.get("id")
    require(isinstance(call_id, str) and bool(call_id), "typed tool call is missing its ID")

    require(result.get("toolCallId") == call_id, "tool result is not linked to the typed tool call")
    require(result.get("toolName") == tool["name"], "tool result names the wrong tool")
    require(result.get("isError") is False, "Golden-equivalent tool execution failed")
    result_content = result.get("content")
    require(isinstance(result_content, list) and len(result_content) == 1, "tool result content shape mismatch")
    require(
        isinstance(result_content[0], dict)
        and result_content[0].get("type") == "text"
        and result_content[0].get("text") == tool["result"],
        "tool result continuation payload mismatch",
    )

    final_content = final.get("content")
    require(isinstance(final_content, list), "final assistant content is not an array")
    require(
        not any(isinstance(block, dict) and block.get("type") == "toolCall" for block in final_content),
        "final assistant emitted an extra tool call",
    )
    text_blocks = [block for block in final_content if isinstance(block, dict) and block.get("type") == "text"]
    require(len(text_blocks) == 1, "final assistant must contain exactly one visible text block")
    final_answer = visible_text(final)
    require(final_answer == contract["final_visible_answer"], "exact visible final-answer oracle failed")

    projection = {
        "sequence": ["assistant_typed_tool_call", "tool_result", "assistant_final_answer"],
        "tool_name": tool["name"],
        "typed_arguments": tool["expected_arguments"],
        "tool_result": tool["result"],
        "final_visible_answer": final_answer,
    }
    return projection


def write_models(path: Path, api_base: str, model: str) -> None:
    def quoted(value: str) -> str:
        return json.dumps(value, ensure_ascii=True)

    path.write_text(
        "\n".join(
            [
                "providers:",
                f"  {PROVIDER}:",
                f"    baseUrl: {quoted(api_base)}",
                "    api: openai-completions",
                "    apiKey: NINFER_GOLDEN_API_KEY",
                "    authHeader: true",
                "    models:",
                f"      - id: {quoted(model)}",
                '        name: "NInfer RTX 4090 Golden-equivalent"',
                "        reasoning: false",
                "        input: [text]",
                "        cost:",
                "          input: 0",
                "          output: 0",
                "          cacheRead: 0",
                "          cacheWrite: 0",
                "        contextWindow: 131072",
                "        maxTokens: 512",
                "        compat:",
                "          supportsStore: false",
                "          supportsDeveloperRole: false",
                "          supportsReasoningEffort: false",
                "          maxTokensField: max_completion_tokens",
                "",
            ]
        ),
        encoding="utf-8",
    )


def write_agent_config(path: Path) -> None:
    path.write_text(
        "disabledProviders:\n"
        "  - native\n"
        "  - claude\n"
        "  - codex\n"
        "  - gemini\n"
        "  - github\n"
        "  - opencode\n"
        "  - cursor\n"
        "  - agents-md\n"
        "tools:\n"
        "  intentTracing: false\n",
        encoding="utf-8",
    )


def parse_events(stdout: str) -> list[Mapping[str, object]]:
    events: list[Mapping[str, object]] = []
    for line_number, line in enumerate(stdout.splitlines(), start=1):
        if not line.strip():
            continue
        try:
            value = json.loads(line)
        except json.JSONDecodeError as error:
            raise QualificationError(f"OMP JSON output line {line_number} is invalid") from error
        require(isinstance(value, dict), f"OMP JSON output line {line_number} is not an object")
        events.append(value)
    require(bool(events), "OMP emitted no JSON events")
    return events


def invoke_omp(
    *,
    executable: str,
    api_base: str,
    api_key: str,
    model: str,
    extension: Path,
    contract: Mapping[str, Any],
    timeout_seconds: int,
) -> tuple[list[Mapping[str, object]], str, float]:
    version_result = subprocess.run(
        [executable, "--version"],
        check=False,
        capture_output=True,
        text=True,
        timeout=30,
    )
    require(version_result.returncode == 0, "omp --version failed")
    omp_version = version_result.stdout.strip()
    require(bool(omp_version), "omp --version returned no identity")

    with tempfile.TemporaryDirectory(prefix="ninfer-omp-golden-") as directory:
        root = Path(directory)
        agent_dir = root / "agent"
        workspace = root / "workspace"
        agent_dir.mkdir(mode=0o700)
        workspace.mkdir(mode=0o700)
        write_models(agent_dir / "models.yml", api_base, model)
        write_agent_config(agent_dir / "config.yml")
        (agent_dir / "mcp.json").write_text(
            '{"mcpServers":{},"disabledServers":[]}\n', encoding="utf-8"
        )
        environment = os.environ.copy()
        for name in tuple(environment):
            if name.startswith("OMP_CODE_MODE_") or name in {
                "OMP_BIN",
                "OMP_PROFILE",
                "OMP_RELEASE_ROOT",
                "OMP_RUNTIME_VARIANT",
                "PI_CONFIG_FILES",
            }:
                environment.pop(name)
        environment.update(
            {
                "PI_CODING_AGENT_DIR": str(agent_dir),
                "NINFER_GOLDEN_API_KEY": api_key,
                "NO_COLOR": "1",
            }
        )
        command = [
            executable,
            "--mode",
            "json",
            "--print",
            "--no-session",
            "--no-title",
            "--no-extensions",
            "--extension",
            str(extension),
            "--no-skills",
            "--no-rules",
            "--no-lsp",
            "--no-pty",
            "--no-tools",
            "--tools",
            contract["tool"]["name"],
            "--auto-approve",
            "--approval-mode",
            "yolo",
            "--model",
            f"{PROVIDER}/{model}",
            "--thinking",
            "off",
            "--system-prompt",
            contract["system_prompt"],
            "--cwd",
            str(workspace),
            "--max-time",
            str(timeout_seconds),
            contract["user_prompt"],
        ]
        started = time.monotonic()
        result = subprocess.run(
            command,
            check=False,
            capture_output=True,
            text=True,
            env=environment,
            timeout=timeout_seconds + 30,
        )
        elapsed = time.monotonic() - started
    if result.returncode != 0:
        detail = next((line.strip() for line in reversed(result.stderr.splitlines()) if line.strip()), "no stderr")
        raise QualificationError(f"OMP exited {result.returncode}: {detail[:400]}")
    return parse_events(result.stdout), omp_version, elapsed


def write_receipt(path: Path, receipt: Mapping[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f"{path.name}.{os.getpid()}.tmp")
    try:
        temporary.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def lower_sha(value: str) -> str:
    if not SHA256_RE.fullmatch(value):
        raise argparse.ArgumentTypeError("expected a lower-case SHA-256")
    return value


def source_sha(value: str) -> str:
    if not SOURCE_RE.fullmatch(value):
        raise argparse.ArgumentTypeError("expected a lower-case 40-character source commit")
    return value


def run(args: argparse.Namespace) -> dict[str, object]:
    contract, contract_file_sha256, contract_sha256 = load_contract(args.contract)
    extension_sha256 = digest_bytes(args.extension.read_bytes())
    key = read_key(args.api_key_file)
    api_base, status_url = endpoint_urls(args.base_url)
    status = request_json(status_url, key)
    identity = validate_status(
        status,
        source_commit=args.expect_source_commit,
        binary_sha256=args.expect_binary_sha256,
        model_artifact_sha256=args.expect_model_artifact_sha256,
        config_sha256=args.expect_config_sha256,
        deployment_profile=args.expect_deployment_profile,
    )
    events, omp_version, wall_seconds = invoke_omp(
        executable=args.omp,
        api_base=api_base,
        api_key=key,
        model=args.model,
        extension=args.extension,
        contract=contract,
        timeout_seconds=args.timeout_seconds,
    )
    projection = validate_transcript(events, contract)
    projection_sha256 = digest_bytes(canonical_bytes(projection))
    return {
        "artifact_type": RECEIPT_ARTIFACT,
        "schema_version": 1,
        "status": "passed",
        "qualified_utc": datetime.now(timezone.utc).isoformat(),
        "identity": identity,
        "contract": {
            "contract_id": contract["contract_id"],
            "contract_file_sha256": contract_file_sha256,
            "contract_sha256": contract_sha256,
            "extension_sha256": extension_sha256,
            "fixture_provenance": contract["fixture_provenance"],
            "historical_private_corpus_reused": False,
        },
        "omp": {
            "version": omp_version,
            "transport": "openai-completions",
            "tool_name": contract["tool"]["name"],
            "wall_seconds": round(wall_seconds, 6),
        },
        "oracles": {
            "typed_tool_invocation": "passed",
            "typed_argument_oracle": "passed",
            "tool_result_continuation": "passed",
            "exact_visible_final_answer": "passed",
            "expected_arguments": contract["tool"]["expected_arguments"],
            "final_visible_answer": contract["final_visible_answer"],
            "final_visible_answer_sha256": digest_bytes(contract["final_visible_answer"].encode("utf-8")),
            "transcript_projection_sha256": projection_sha256,
        },
        "raw_transcript_included": False,
    }


def main() -> int:
    default_root = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser()
    parser.add_argument("--omp", default="omp")
    parser.add_argument("--base-url", required=True)
    parser.add_argument("--model", required=True)
    parser.add_argument("--api-key-file", required=True, type=Path)
    parser.add_argument("--expect-source-commit", required=True, type=source_sha)
    parser.add_argument("--expect-binary-sha256", required=True, type=lower_sha)
    parser.add_argument("--expect-model-artifact-sha256", required=True, type=lower_sha)
    parser.add_argument("--expect-config-sha256", required=True, type=lower_sha)
    parser.add_argument("--expect-deployment-profile", required=True)
    parser.add_argument("--contract", type=Path, default=default_root / "golden_equivalent_contract.json")
    parser.add_argument("--extension", type=Path, default=default_root / "golden_equivalent_extension.ts")
    parser.add_argument("--timeout-seconds", type=int, default=600)
    parser.add_argument("--receipt-path", type=Path)
    args = parser.parse_args()
    try:
        require(1 <= args.timeout_seconds <= 1800, "timeout must be between 1 and 1800 seconds")
        receipt = run(args)
        if args.receipt_path is not None:
            write_receipt(args.receipt_path, receipt)
    except (QualificationError, OSError, subprocess.TimeoutExpired, json.JSONDecodeError) as error:
        print(json.dumps({"artifact_type": RECEIPT_ARTIFACT, "schema_version": 1, "status": "failed", "error": str(error)}, sort_keys=True))
        return 1
    print(json.dumps(receipt, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
