from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
import types
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


class TokenizerToolIsolationTest(unittest.TestCase):
    def assert_remote_code_disabled(self, loader) -> None:
        calls: list[tuple[tuple[object, ...], dict[str, object]]] = []

        class AutoTokenizer:
            @staticmethod
            def from_pretrained(*args, **kwargs):
                calls.append((args, kwargs))
                return object()

        transformers = types.ModuleType("transformers")
        setattr(transformers, "AutoTokenizer", AutoTokenizer)
        with mock.patch.dict(sys.modules, {"transformers": transformers}):
            loader(Path("/tmp/tokenizer"))

        self.assertEqual(len(calls), 1)
        _, options = calls[0]
        self.assertIs(options["local_files_only"], True)
        self.assertIs(options["trust_remote_code"], False)
        self.assertIs(options["use_fast"], True)

    def test_ttft_fixture_builder_never_executes_tokenizer_code(self) -> None:
        module = load_module(
            "ttft_build_fixtures_test",
            ROOT / "tools" / "bench" / "ttft" / "build_fixtures.py",
        )
        self.assert_remote_code_disabled(module._load_tokenizer)

    def test_benchmark_corpus_builder_never_executes_tokenizer_code(self) -> None:
        module = load_module(
            "make_bench_corpus_test",
            ROOT / "tools" / "bench" / "make_bench_corpus.py",
        )
        self.assert_remote_code_disabled(module.load_tokenizer)


if __name__ == "__main__":
    unittest.main()
