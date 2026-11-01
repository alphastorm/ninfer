import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import zlib


LAUNCHER = Path(__file__).resolve().parents[1] / "tools/release/deterministic-nvcc.py"


class CudaDeterminismTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.recorder = self.root / "record.py"
        self.recorder.write_text(
            "import json, sys\n"
            "print(json.dumps(sys.argv[1:]))\n"
            "raise SystemExit(23 if '--fail' in sys.argv else 0)\n")

    def invoke(self, root, *args):
        # Python + recorder models a caller-supplied compiler launcher. The nvcc
        # argument must remain ahead of the compiler flags for that chain to work.
        return subprocess.run(
            [sys.executable, "-B", str(LAUNCHER), "--source-root", str(root), "--",
             sys.executable, str(self.recorder), "nvcc", *map(str, args)],
            capture_output=True, text=True)

    def source(self, root, relative):
        source = root / relative
        source.parent.mkdir(parents=True, exist_ok=True)
        source.write_text("// source identity fixture\n")
        return source

    def test_source_relative_seed_and_same_basename_isolation(self):
        seeds = []
        for tree, relative in (("first tree", "left/kernel.cu"),
                               ("second tree", "left/kernel.cu"),
                               ("second tree", "right/kernel.cu")):
            root = self.root / tree
            source = self.source(root, relative)
            result = self.invoke(root, "-c", source, "-o", "kernel.o")
            self.assertEqual(result.returncode, 0, result.stderr)
            command = json.loads(result.stdout)
            self.assertEqual(command[:5], ["nvcc", "-c", str(source), "-o", "kernel.o"])
            self.assertEqual(command[-1], "--objdir-as-tempdir")
            seed = command[-2].removeprefix("--frandom-seed=")
            self.assertEqual(seed, relative)
            seeds.append(seed)
        self.assertEqual(seeds[0], seeds[1])
        self.assertNotEqual(seeds[1], seeds[2])

    def test_repository_source_seeds_are_distinct(self):
        repository = LAUNCHER.parents[2]
        seeds = {}
        for directory in ("src", "tests", "tools"):
            for source in sorted((repository / directory).rglob("*.cu")):
                relative = source.relative_to(repository).as_posix()
                seed = zlib.crc32(relative.encode("utf-8"))
                self.assertNotIn(seed, seeds, f"seed collision: {seeds.get(seed)} and {relative}")
                seeds[seed] = relative
        self.assertTrue(seeds)

    def test_compiler_failure_and_source_less_passthrough(self):
        result = self.invoke(self.root, "-dlink", "kernel.o", "--fail")
        self.assertEqual(result.returncode, 23)
        self.assertEqual(json.loads(result.stdout), ["nvcc", "-dlink", "kernel.o", "--fail"])

    def test_outside_root_and_multiple_sources_refuse_before_compiler(self):
        source = self.source(self.root / "source", "left/kernel.cu")
        other = self.source(self.root / "source", "right/kernel.cu")
        for root, args in ((self.root / "elsewhere", [source]),
                           (self.root / "source", [source, other])):
            result = self.invoke(root, "-c", *args)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, "")


if __name__ == "__main__":
    unittest.main()
