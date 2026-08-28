from __future__ import annotations

import importlib.util
from pathlib import Path
import tempfile
import unittest
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "packaging" / "windows" / "qwen38-4090-v0.1" / "golden_equivalent.py"
CONTRACT = RUNNER.with_name("golden_equivalent_contract.json")
SPEC = importlib.util.spec_from_file_location("ninfer_golden_equivalent", RUNNER)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class GoldenEquivalentTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.contract, cls.file_sha256, cls.contract_sha256 = MODULE.load_contract(CONTRACT)

    def transcript(self) -> list[dict[str, Any]]:
        tool = self.contract["tool"]
        call_id = "call_source_controlled_golden"
        return [
            {"type": "message_start", "message": {"role": "user", "content": []}},
            {
                "type": "message_end",
                "message": {
                    "role": "assistant",
                    "stopReason": "toolUse",
                    "content": [
                        {
                            "type": "toolCall",
                            "id": call_id,
                            "name": tool["name"],
                            "arguments": dict(tool["expected_arguments"]),
                        }
                    ],
                },
            },
            {
                "type": "message_end",
                "message": {
                    "role": "toolResult",
                    "toolCallId": call_id,
                    "toolName": tool["name"],
                    "content": [{"type": "text", "text": tool["result"]}],
                    "isError": False,
                },
            },
            {
                "type": "message_end",
                "message": {
                    "role": "assistant",
                    "stopReason": "stop",
                    "content": [{"type": "text", "text": self.contract["final_visible_answer"]}],
                },
            },
        ]

    def test_source_controlled_contract_and_complete_oracle_pass(self) -> None:
        self.assertFalse(self.contract["historical_private_corpus_reused"])
        self.assertIn("historical private corpus was not reused", self.contract["fixture_provenance"])
        self.assertRegex(self.file_sha256, r"^[0-9a-f]{64}$")
        self.assertRegex(self.contract_sha256, r"^[0-9a-f]{64}$")
        projection = MODULE.validate_transcript(self.transcript(), self.contract)
        self.assertEqual(
            projection["sequence"],
            ["assistant_typed_tool_call", "tool_result", "assistant_final_answer"],
        )
        self.assertEqual(projection["final_visible_answer"], self.contract["final_visible_answer"])

    def test_typed_argument_oracle_rejects_type_substitution(self) -> None:
        cases = {
            "integer_as_string": {"city": "Paris", "days": "3", "metric": True},
            "boolean_as_integer": {"city": "Paris", "days": 3, "metric": 1},
            "string_as_number": {"city": 7, "days": 3, "metric": True},
            "additional_property": {"city": "Paris", "days": 3, "metric": True, "unit": "C"},
        }
        for name, arguments in cases.items():
            with self.subTest(name=name):
                events = self.transcript()
                events[1]["message"]["content"][0]["arguments"] = arguments
                with self.assertRaisesRegex(MODULE.QualificationError, "typed argument oracle"):
                    MODULE.validate_transcript(events, self.contract)

    def test_observed_omp_arguments_fail_before_fixed_domain_arguments_pass(self) -> None:
        expected = self.contract["tool"]["expected_arguments"]
        observed = {
            "city": "Paris",
            "days": "3",
            "metric": "true",
            "i": "Looking up fixed weather",
        }
        self.assertFalse(MODULE.exact_arguments(observed, expected))
        self.assertTrue(MODULE.exact_arguments(dict(expected), expected))

    def test_isolated_omp_config_disables_intent_argument_injection(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "config.yml"
            MODULE.write_agent_config(path)
            text = path.read_text(encoding="utf-8")
        self.assertIn("tools:\n  intentTracing: false\n", text)

    def test_tool_result_must_link_and_continue_exactly(self) -> None:
        broken_link = self.transcript()
        broken_link[2]["message"]["toolCallId"] = "call_other"
        with self.assertRaisesRegex(MODULE.QualificationError, "not linked"):
            MODULE.validate_transcript(broken_link, self.contract)

        changed_result = self.transcript()
        changed_result[2]["message"]["content"][0]["text"] += " changed"
        with self.assertRaisesRegex(MODULE.QualificationError, "continuation payload mismatch"):
            MODULE.validate_transcript(changed_result, self.contract)

        wrong_order = self.transcript()
        wrong_order[1], wrong_order[2] = wrong_order[2], wrong_order[1]
        with self.assertRaisesRegex(MODULE.QualificationError, "continuation order"):
            MODULE.validate_transcript(wrong_order, self.contract)

    def test_visible_final_answer_is_exact(self) -> None:
        for suffix in ("\n", ".", " extra"):
            with self.subTest(suffix=repr(suffix)):
                events = self.transcript()
                events[3]["message"]["content"][0]["text"] += suffix
                with self.assertRaisesRegex(MODULE.QualificationError, "final-answer oracle"):
                    MODULE.validate_transcript(events, self.contract)

        extra_block = self.transcript()
        extra_block[3]["message"]["content"].append({"type": "text", "text": ""})
        with self.assertRaisesRegex(MODULE.QualificationError, "exactly one visible text block"):
            MODULE.validate_transcript(extra_block, self.contract)

    def test_status_identity_binds_source_binary_model_and_config(self) -> None:
        source = "1" * 40
        binary = "2" * 64
        model = "3" * 64
        config = "4" * 64
        profile = "sf-qwen38-4090-v0.1"
        status = {
            "artifact_type": "ninfer_server_status",
            "schema_version": 1,
            "status": "ok",
            "identity": {
                "upstream_base_sha": "5" * 40,
                "patch_stack_sha": source,
                "source_dirty": False,
                "build_profile": profile,
                "build_type": "Release",
                "cuda_architecture": "89",
                "deployment_profile": profile,
                "binary_sha256": binary,
                "model_artifact_sha256": model,
                "config_sha256": config,
            },
        }
        identity = MODULE.validate_status(
            status,
            source_commit=source,
            binary_sha256=binary,
            model_artifact_sha256=model,
            config_sha256=config,
            deployment_profile=profile,
        )
        self.assertEqual(identity["patch_stack_sha"], source)

        status["identity"]["binary_sha256"] = "6" * 64
        with self.assertRaisesRegex(MODULE.QualificationError, "binary_sha256"):
            MODULE.validate_status(
                status,
                source_commit=source,
                binary_sha256=binary,
                model_artifact_sha256=model,
                config_sha256=config,
                deployment_profile=profile,
            )


if __name__ == "__main__":
    unittest.main()
