from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[1]
RUNNER = REPOSITORY / "tools" / "bench" / "run_sm120_mtp3_profile.sh"
WORKFLOW = REPOSITORY / "tools" / "bench" / "run_sm120_mtp3_workflow.sh"
SOURCE_SHA = "0123456789abcdef0123456789abcdef01234567"
UPSTREAM_SHA = "89abcdef0123456789abcdef0123456789abcdef"
TOOLCHAIN_IMAGE_ID = "sha256:" + "a" * 64


def write_executable(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(textwrap.dedent(content).lstrip(), encoding="utf-8")
    path.chmod(0o755)


def install_tool_mocks(root: Path) -> Path:
    tools = root / "mock-bin"
    tools.mkdir()
    write_executable(
        tools / "git",
        rf"""
        #!/usr/bin/env bash
        set -euo pipefail
        if [[ ${{1:-}} == -C ]]; then
          shift 2
        fi
        case ${{1:-}} in
          status)
            if [[ ${{MOCK_SOURCE_DIRTY:-0}} == 1 ]]; then
              printf ' M tools/bench/run_sm120_mtp3_profile.sh\n'
            fi
            ;;
          rev-parse)
            printf '{SOURCE_SHA}\n'
            ;;
          cat-file)
            ;;
          merge-base)
            [[ $MOCK_ANCESTRY_FAIL != 1 ]]
            ;;
          *)
            echo "unexpected git invocation: $*" >&2
            exit 90
            ;;
        esac
        """,
    )
    write_executable(
        tools / "cmake",
        r"""
        #!/usr/bin/env bash
        set -euo pipefail
        printf 'cmake %s\n' "$*" >>"$MOCK_TOOL_LOG"
        if [[ ${1:-} == -N ]]; then
          printf 'MOCK_CMAKE_CACHE=1\n'
          exit 0
        fi
        if [[ ${1:-} != --build ]]; then
          build=
          while (($#)); do
            if [[ $1 == -B ]]; then
              build=$2
              break
            fi
            shift
          done
          [[ -n $build ]]
          printf 'CMAKE_HOME_DIRECTORY:INTERNAL=%s\n' "$MOCK_SOURCE_ROOT" >"$build/CMakeCache.txt"
          exit 0
        fi
        build=$2
        mkdir -p "$build/bench" "$build/tests"
        for name in ninfer_q4_linear_swiglu_bench ninfer_q5_linear_add_bench; do
          cat >"$build/bench/$name" <<'SH'
        #!/usr/bin/env bash
        printf '%s %s\n' "$(basename "$0")" "$*" >>"$MOCK_TOOL_LOG"
        printf 'mock timing\n'
        SH
          chmod +x "$build/bench/$name"
        done
        cat >"$build/bench/ninfer_qwen3_6_27b_post_mixer_bench" <<'SH'
        #!/usr/bin/env bash
        set -euo pipefail
        printf 'post-mixer %s\n' "$*" >>"$MOCK_TOOL_LOG"
        state=q4-produced
        candidate=public
        scope=q5
        describe=0
        profile=0
        while (($#)); do
          case $1 in
            --activation-state) state=$2; shift 2 ;;
            --candidate) candidate=$2; shift 2 ;;
            --measure) scope=$2; shift 2 ;;
            --describe) describe=1; shift ;;
            --profile-q5) profile=1; shift ;;
            --warmup|--repeat) shift 2 ;;
            *) shift ;;
          esac
        done
        if ((describe)); then
          printf '{"schema_version":1,"workload":"qwen27-post-mixer-t4"}\n'
          exit 0
        fi
        if ((profile)); then
          printf 'PROFILE mock\n'
          exit 0
        fi
        median=80
        [[ $state == cold ]] && median=100
        [[ $state == hot ]] && median=70
        [[ $candidate == mma-r64-c16 ]] && median=104
        [[ $scope == chain ]] && median=200
        printf '{"schema_version":1,"workload":"qwen27-post-mixer-t4","activation_state":"%s","candidate":"%s","timed_scope":"%s","median_us":%s,"min_us":%s,"p95_us":%s}\n' \
          "$state" "$candidate" "$scope" "$median" "$median" "$median"
        SH
        chmod +x "$build/bench/ninfer_qwen3_6_27b_post_mixer_bench"
        cat >"$build/bench/ninfer_qwen3_6_27b_mtp_round_bench" <<'SH'
        #!/usr/bin/env bash
        printf 'mtp-round %s\n' "$*" >>"$MOCK_TOOL_LOG"
        printf 'format,ninfer_qwen3_6_27b_mtp_round_bench_v1\nmtp_round_mean_ms,1.0\n'
        SH
        chmod +x "$build/bench/ninfer_qwen3_6_27b_mtp_round_bench"
        for name in ninfer_linear_swiglu_q4_a16_test ninfer_linear_add_q5_a16_test; do
          cat >"$build/tests/$name" <<'SH'
        #!/usr/bin/env bash
        printf '%s\n' "$(basename "$0")" >>"$MOCK_TOOL_LOG"
        printf 'OK mock oracle\n'
        SH
          chmod +x "$build/tests/$name"
        done
        """,
    )
    write_executable(
        tools / "cuobjdump",
        r"""
        #!/usr/bin/env bash
        if [[ ${1:-} == --list-elf ]]; then
          printf 'ELF file sm_120a\n'
        else
          printf 'mock resource usage\n'
        fi
        """,
    )
    write_executable(
        tools / "ncu",
        r"""
        #!/usr/bin/env bash
        set -euo pipefail
        printf 'ncu %s\n' "$*" >>"$MOCK_TOOL_LOG"
        if [[ ${1:-} == --version ]]; then
          printf 'mock ncu\n'
          exit 0
        fi
        if [[ ${1:-} == --list-sections ]]; then
          printf '%s\n' LaunchStats Occupancy SpeedOfLight MemoryWorkloadAnalysis SchedulerStats WarpStateStats SourceCounters
          exit 0
        fi
        if [[ ${1:-} == --import ]]; then
          printf 'ID,Kernel Name,Metric Name,Metric Unit,Metric Value\n'
          printf '1,mock,gpu__time_duration.sum,usecond,1\n'
          exit 0
        fi
        if [[ ${MOCK_NCU_COUNTER_DENIED:-0} == 1 ]]; then
          printf '==ERROR== ERR_NVGPUCTRPERM permission denied\n'
          exit 1
        fi
        export_path=
        while (($#)); do
          if [[ $1 == --export ]]; then
            export_path=$2
            shift 2
          else
            shift
          fi
        done
        [[ -n $export_path ]]
        printf 'mock report\n' >"$export_path.ncu-rep"
        printf 'mock ncu capture\n'
        """,
    )
    write_executable(
        tools / "nsys",
        r"""
        #!/usr/bin/env bash
        set -euo pipefail
        printf 'nsys %s\n' "$*" >>"$MOCK_TOOL_LOG"
        if [[ ${1:-} == --version ]]; then
          printf 'mock nsys\n'
          exit 0
        fi
        if [[ ${1:-} == profile ]]; then
          shift
          output=
          while (($#)); do
            if [[ $1 == -o ]]; then
              output=$2
              shift 2
            else
              shift
            fi
          done
          [[ -n $output ]]
          printf 'mock report\n' >"$output.nsys-rep"
          printf 'mock nsys profile\n'
          exit 0
        fi
        if [[ ${1:-} == stats ]]; then
          printf 'Kernel Name,Total Time\nmock,1\n'
          exit 0
        fi
        exit 90
        """,
    )
    write_executable(
        tools / "nvidia-smi",
        r"""
        #!/usr/bin/env bash
        printf 'nvidia-smi %s\n' "$*" >>"$MOCK_TOOL_LOG"
        printf 'name, uuid, driver_version\nMock 5090, GPU-mock, 999.0\n'
        """,
    )
    write_executable(
        tools / "nvcc",
        r"""
        #!/usr/bin/env bash
        printf 'mock nvcc 13.1\n'
        """,
    )
    write_executable(
        tools / "timeout",
        r"""
        #!/usr/bin/env bash
        shift
        exec "$@"
        """,
    )
    return tools


def create_fixture(root: Path) -> dict[str, Path]:
    source = (root / "source").resolve()
    source.mkdir(parents=True)
    build = source / "build" / "profile"
    prepare = source / "profiles" / "prepared"
    artifact = source / "out" / "qwen3_6_27b.ninfer"
    output = source / "profiles" / "sm120-mtp3" / "run"
    build.mkdir(parents=True)
    prepare.mkdir(parents=True)
    artifact.parent.mkdir(parents=True)
    artifact.write_bytes(b"mock artifact")
    tools = source / "tools" / "bench"
    tools.mkdir(parents=True)
    for script in (RUNNER, WORKFLOW):
        shutil.copy2(script, tools / script.name)
        (tools / script.name).chmod(0o755)
    write_executable(tools / "run_gpu_profile_lease.sh", "#!/usr/bin/env bash\nexit 99\n")
    write_executable(
        tools / "run_sm120_q4_mtp3_profile.sh",
        r"""
        #!/usr/bin/env bash
        set -euo pipefail
        printf 'q4-packet %s\n' "$*" >>"$MOCK_TOOL_LOG"
        mkdir -p "$3"
        printf '{"schema_version":1}\n' >"$3/summary.json"
        """,
    )
    write_executable(
        tools / "summarize_ncu_q4.py",
        "#!/usr/bin/env python3\nraise SystemExit('not executed by mock')\n",
    )
    return {
        "source": source,
        "build": build,
        "prepare": prepare,
        "artifact": artifact,
        "output": output,
        "runner": tools / RUNNER.name,
        "tool_log": root / "tools.log",
    }


class Sm120Mtp3ProfileRunnerTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary_directory.cleanup)
        self.root = Path(self.temporary_directory.name)
        self.paths = create_fixture(self.root)
        self.mock_tools = install_tool_mocks(self.root)
        self.environment = os.environ.copy()
        self.environment.update(
            {
                "PATH": f"{self.mock_tools}:{self.environment['PATH']}",
                "PYTHON": sys.executable,
                "NINFER_CONTROLLER_ID": "stable-controller",
                "NINFER_TARGET_ID": "mock-5090",
                "NINFER_EXPECTED_SOURCE_SHA": SOURCE_SHA,
                "NINFER_UPSTREAM_BASE_SHA": UPSTREAM_SHA,
                "NINFER_ARTIFACT_EXPECTED_SHA256": hashlib.sha256(b"mock artifact").hexdigest(),
                "NINFER_TOOLCHAIN_IMAGE_ID": TOOLCHAIN_IMAGE_ID,
                "NINFER_NCU_TIMEOUT_SECONDS": "5",
                "NINFER_NSYS_TIMEOUT_SECONDS": "5",
                "MOCK_SOURCE_ROOT": str(self.paths["source"]),
                "MOCK_TOOL_LOG": str(self.paths["tool_log"]),
                "MOCK_ANCESTRY_FAIL": "0",
            }
        )

    def run_prepare(
        self, *, source_dirty: bool = False, ancestry_invalid: bool = False
    ) -> subprocess.CompletedProcess[str]:
        environment = {
            **self.environment,
            "MOCK_SOURCE_DIRTY": "1" if source_dirty else "0",
            "MOCK_ANCESTRY_FAIL": "1" if ancestry_invalid else "0",
        }
        return subprocess.run(
            [
                self.paths["runner"],
                "prepare",
                self.paths["source"],
                self.paths["build"],
                self.paths["artifact"],
                self.paths["prepare"],
            ],
            text=True,
            capture_output=True,
            env=environment,
            check=False,
        )

    def run_capture(self, *, counter_denied: bool = False) -> subprocess.CompletedProcess[str]:
        environment = {
            **self.environment,
            "MOCK_SOURCE_DIRTY": "0",
            "MOCK_NCU_COUNTER_DENIED": "1" if counter_denied else "0",
        }
        return subprocess.run(
            [
                self.paths["runner"],
                "capture",
                self.paths["source"],
                self.paths["build"],
                self.paths["artifact"],
                self.paths["prepare"],
                self.paths["output"],
            ],
            text=True,
            capture_output=True,
            env=environment,
            check=False,
        )

    def tool_log_lines(self) -> list[str]:
        path = self.paths["tool_log"]
        return path.read_text(encoding="utf-8").splitlines() if path.exists() else []

    def test_prepares_exact_build_without_gpu_tools(self) -> None:
        completed = self.run_prepare()

        self.assertEqual(completed.returncode, 0, completed.stderr + completed.stdout)
        self.assertTrue((self.paths["prepare"] / "prepare-complete.txt").is_file())
        manifest = json.loads((self.paths["prepare"] / "manifest.json").read_text(encoding="utf-8"))
        self.assertTrue(manifest["source_clean"])
        self.assertEqual(manifest["source_sha"], SOURCE_SHA)
        self.assertEqual(manifest["toolchain_image_id"], TOOLCHAIN_IMAGE_ID)
        log = self.tool_log_lines()
        self.assertTrue(any(line.startswith("cmake -S ") for line in log))
        self.assertTrue(any(line.startswith("cmake --build ") for line in log))
        self.assertFalse(any(line.startswith(("ncu ", "nsys ", "nvidia-smi ")) for line in log))

    def test_captures_complete_fixed_packet_after_counter_gate(self) -> None:
        prepared = self.run_prepare()
        self.assertEqual(prepared.returncode, 0, prepared.stderr + prepared.stdout)
        self.paths["tool_log"].write_text("", encoding="utf-8")

        completed = self.run_capture()

        output = self.paths["output"]
        self.assertEqual(completed.returncode, 0, completed.stderr + completed.stdout)
        self.assertTrue((output / "packet-complete.txt").is_file())
        self.assertTrue((output / "ncu" / "counter-access-verified.txt").is_file())
        self.assertTrue((output / "nsys" / "mtp3-round.nsys-rep").is_file())
        for name in (
            "q5-split2-cold.ncu-rep",
            "q5-split2-q4-produced.ncu-rep",
            "q5-c16-q4-produced.ncu-rep",
        ):
            self.assertGreater((output / "ncu" / name).stat().st_size, 0, name)
        self.assertTrue((output / "ncu" / "q4-packet" / "summary.json").is_file())
        manifest = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
        self.assertTrue(manifest["source_clean"])
        self.assertEqual(manifest["counter_access_gate"], "q5-split2-cold")
        summary = json.loads((output / "timings" / "summary.json").read_text(encoding="utf-8"))
        self.assertAlmostEqual(summary["ratios"]["cold_over_q4_produced"], 1.25)
        self.assertAlmostEqual(summary["ratios"]["c16_over_public_q4_produced"], 1.3)
        self.assertEqual(summary["c16_triage"], "20-to-50-percent-slower")
        log = self.tool_log_lines()
        q4_gate = log.index("ninfer_linear_swiglu_q4_a16_test")
        q5_gate = log.index("ninfer_linear_add_q5_a16_test")
        counter_gate = next(i for i, line in enumerate(log) if line.startswith("ncu --profile-from-start"))
        first_timing = next(i for i, line in enumerate(log) if line.startswith("post-mixer --describe"))
        self.assertLess(q4_gate, q5_gate)
        self.assertLess(q5_gate, counter_gate)
        self.assertLess(counter_gate, first_timing)
        self.assertFalse(any(line.startswith("cmake --build ") for line in log))

    def test_rejects_any_uncommitted_source_change_before_build(self) -> None:
        completed = self.run_prepare(source_dirty=True)

        self.assertEqual(completed.returncode, 2)
        self.assertIn("completely clean committed worktree", completed.stderr)
        self.assertFalse((self.paths["prepare"] / "prepare-complete.txt").exists())
        self.assertFalse(any(line.startswith("cmake ") for line in self.tool_log_lines()))

    def test_rejects_non_ancestor_upstream_before_build(self) -> None:
        completed = self.run_prepare(ancestry_invalid=True)

        self.assertEqual(completed.returncode, 2)
        self.assertIn("not an ancestor of NINFER_EXPECTED_SOURCE_SHA", completed.stderr)
        self.assertFalse((self.paths["prepare"] / "prepare-complete.txt").exists())
        self.assertFalse(any(line.startswith("cmake ") for line in self.tool_log_lines()))

    def test_rejects_missing_capture_tool_before_build(self) -> None:
        (self.mock_tools / "nsys").unlink()

        completed = self.run_prepare()

        self.assertEqual(completed.returncode, 2)
        self.assertIn("required preparation command is unavailable: nsys", completed.stderr)
        self.assertFalse((self.paths["prepare"] / "prepare-complete.txt").exists())
        self.assertFalse(any(line.startswith("cmake ") for line in self.tool_log_lines()))

    def test_counter_permission_failure_blocks_later_profiling(self) -> None:
        prepared = self.run_prepare()
        self.assertEqual(prepared.returncode, 0, prepared.stderr + prepared.stdout)
        self.paths["tool_log"].write_text("", encoding="utf-8")

        completed = self.run_capture(counter_denied=True)

        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("performance-counter access is unavailable", completed.stderr)
        self.assertFalse((self.paths["output"] / "ncu" / "counter-access-verified.txt").exists())
        self.assertFalse((self.paths["output"] / "timings" / "summary.json").exists())
        self.assertFalse(any(line.startswith("nsys profile ") for line in self.tool_log_lines()))


if __name__ == "__main__":
    unittest.main()
