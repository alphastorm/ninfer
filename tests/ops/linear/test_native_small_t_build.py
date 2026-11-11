#!/usr/bin/env python3
"""GPU-free checks for the native verify configure guard and identity proof."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

import native_small_t_default_identity as IDENTITY

ROOT = Path(__file__).resolve().parents[3]


class NativeSmallTBuild(unittest.TestCase):
    def test_profile_and_architecture_matrix(self):
        cmake = shutil.which("cmake")
        if cmake is None:
            self.fail("CMake is required for configure-guard tests")
        source = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        block = "option(NINFER_NATIVE_SMALL_T_TC" + source.split(
            "option(NINFER_NATIVE_SMALL_T_TC", 1)[1].split("set(CMAKE_CXX_STANDARD", 1)[0]
        scratch = ROOT / ".agent-tmp" / "native-small-t"
        scratch.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=scratch) as directory:
            script = Path(directory) / "guard.cmake"
            script.write_text(block, encoding="utf-8")
            for arch in ("86", "89", "120a", "86;89"):
                for enabled in ("ON", "OFF"):
                    for profile, has_tc in (("ci-sm86", False), ("ci-sm86-tc", True),
                                             ("native-tc-a8-rtx3090", True),
                                             ("native-tcfoo-rtx3090", False)):
                        with self.subTest(arch=arch, enabled=enabled, profile=profile):
                            result = subprocess.run(
                                [cmake, f"-DCMAKE_CUDA_ARCHITECTURES={arch}",
                                 f"-DNINFER_NATIVE_SMALL_T_TC={enabled}",
                                 f"-DNINFER_BUILD_PROFILE={profile}", "-P", str(script)],
                                text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                            expected = ((arch in ("86", "89") and has_tc)
                                        if enabled == "ON" else not has_tc)
                            self.assertEqual(result.returncode == 0, expected, result.stderr)

    def test_identity_preserves_semantic_tokens(self):
        self.assertEqual(IDENTITY.normalized('# 1 "a"\n\nif (t == 8) return x;\n'),
                         'if (t == 8) return x;\n')
        self.assertNotEqual(IDENTITY.normalized('if (t == 8) return x;'),
                            IDENTITY.normalized('if (t == 16) return x;'))

    def test_identity_refuses_numerical_options(self):
        for option in ("NINFER_NATIVE_SMALL_T_TC", "NINFER_NATIVE_PREFILL_GROUP64_A8"):
            with self.subTest(option=option), self.assertRaises(ValueError):
                IDENTITY.preprocess_command({"arguments": ["nvcc", f"-D{option}=1"]},
                                            Path("/src"), Path("/base"))

    def test_identity_uses_base_headers_and_keeps_flags(self):
        command = IDENTITY.preprocess_command(
            {"arguments": ["nvcc", "-I/tree/src", "-I/tree/build/generated", "-lineinfo",
                           "-arch=sm_86", "-c", "/tree/src/route.cu", "-o", "route.o"]},
            Path("/tree"), Path("/base"))
        self.assertEqual(command, ["nvcc", "-I/base/src", "-I/tree/build/generated", "-lineinfo",
                                   "-arch=sm_86", "/base/src/route.cu", "-E",
                                   '-D__FILE__="source"', "-D__LINE__=0"])


if __name__ == "__main__":
    unittest.main()
