from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest


REPOSITORY = Path(__file__).resolve().parents[1]


class Sm120MasterProfileTests(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.source = self.root / 'source'
        self.source.mkdir()
        tools = self.source / 'tools/bench'
        tools.mkdir(parents=True)
        for name in ('run_sm120_mtp3_profile.sh', 'run_sm120_mtp3_workflow.sh',
                     'run_sm120_q4_mtp3_profile.sh', 'summarize_ncu_q4.py'):
            shutil.copy2(REPOSITORY / 'tools/bench' / name, tools / name)
            (tools / name).chmod(0o755)
        corpus = self.source / 'bench/fixtures/bench_corpus.ids'
        corpus.parent.mkdir(parents=True)
        corpus.write_text('42 43 44\n', encoding='utf-8')
        self.git('init', '--quiet')
        self.git('config', 'user.name', 'NInfer Test')
        self.git('config', 'user.email', 'ninfer-test@example.invalid')
        self.git('add', '.')
        self.git('commit', '--quiet', '-m', 'profile fixture')
        head = self.git('rev-parse', 'HEAD')
        self.build = self.root / 'build'
        self.prepare = self.root / 'prepared'
        self.output = self.root / 'result'
        self.build.mkdir()
        self.prepare.mkdir()
        self.artifact = self.root / 'model.ninfer'
        self.artifact.write_bytes(b'model fixture')
        self.log = self.root / 'calls.jsonl'
        self.bin = self.root / 'bin'
        self.bin.mkdir()
        self.stub = self.bin / 'stub.py'
        self.stub.write_text(textwrap.dedent(r'''
            import csv
            import json
            import os
            from pathlib import Path
            import sys
            name = Path(sys.argv[0]).name
            args = sys.argv[1:]
            with Path(os.environ['MOCK_LOG']).open('a', encoding='utf-8') as stream:
                stream.write(json.dumps([name, *args]) + '\n')
            def value(flag):
                return args[args.index(flag) + 1]
            if name == 'cmake':
                if args[0] == '-N':
                    print('cache fixture')
                elif args[0] != '--build':
                    build = Path(value('-B'))
                    (build / 'CMakeCache.txt').write_text('CMAKE_HOME_DIRECTORY:INTERNAL=' + value('-S') + '\n')
                else:
                    build = Path(args[1])
                    for target in args[args.index('--target') + 1:]:
                        folder = build / ('tests' if target.endswith('_test') else 'bench')
                        folder.mkdir(exist_ok=True)
                        executable = folder / target
                        executable.write_bytes(Path(os.environ['MOCK_STUB']).read_bytes())
                        executable.chmod(0o755)
            elif name == 'cuobjdump':
                print('ELF file sm_120a')
            elif name == 'ncu':
                if '--list-sections' in args:
                    print('LaunchStats\nSpeedOfLight\nMemoryWorkloadAnalysis')
                elif '--import' in args:
                    kernel = ('q4_linear_swiglu_gemv_pair_kernel' if 'q4-m1' in value('--import') else
                              'q4_a16_sliced_k_mma_kernel' if 'q4-m4' in value('--import') else 'q5_a16_kernel')
                    writer = csv.writer(sys.stdout)
                    writer.writerow(['ID', 'Kernel Name', 'gpu__time_duration.sum', 'dram__bytes.sum.per_second'])
                    writer.writerow(['', '', 'us', 'Gbyte/s'])
                    writer.writerow([1, kernel, 100, 1000])
                elif os.environ.get('MOCK_COUNTER_DENIED') == '1':
                    print('ERR_NVGPUCTRPERM')
                    sys.exit(1)
                else:
                    Path(value('--export') + '.ncu-rep').write_text('fixture report')
            elif name == 'nsys':
                if '--help' in args:
                    sys.exit(64 if os.environ.get('MOCK_HELP_FAIL') == '1' else 0)
                if args[0] == 'stats':
                    print('Total Time (ns),Name')
                    print('12000000,q5_a16_kernel')
                    if os.environ.get('MOCK_GRAPH_BAD') != '1':
                        print('3000000,q4_a16_sliced_k_mma_kernel')
                else:
                    Path(value('-o') + '.nsys-rep').write_text('fixture graph report')
            elif name.startswith('ninfer_'):
                if any(flag in args for flag in ('--candidate', '--activation-state', '--route', '--describe')):
                    raise SystemExit('obsolete fork-only flag')
                if '--csv-out' in args:
                    with Path(value('--csv-out')).open('w', newline='') as stream:
                        writer = csv.writer(stream)
                        writer.writerow(['t', 'execution', 'median_us'])
                        for width in value('--t-sweep').split(','):
                            writer.writerow([width, value('--execution'), 100])
                print('fixture timing')
            elif name in ('nvidia-smi', 'nvcc'):
                print('fixture tool inventory')
            else:
                raise SystemExit('unknown fixture command ' + name)
        ''').lstrip(), encoding='utf-8')
        self.stub.write_text(f'#!{sys.executable}\n' + self.stub.read_text(encoding='utf-8'), encoding='utf-8')
        for name in ('cmake', 'cuobjdump', 'ncu', 'nsys', 'nvidia-smi', 'nvcc'):
            shutil.copy2(self.stub, self.bin / name)
            (self.bin / name).chmod(0o755)
        python = self.bin / 'python3.11'
        python.write_text(f'#!/bin/sh\nif [ "$1" = -c ]; then printf "3.11\\n"; else exec "{sys.executable}" "$@"; fi\n', encoding='utf-8')
        python.chmod(0o755)
        timeout = self.bin / 'timeout'
        timeout.write_text('#!/bin/sh\nshift\nexec "$@"\n', encoding='utf-8')
        timeout.chmod(0o755)
        self.environment = dict(os.environ, PATH=f'{self.bin}:{os.environ["PATH"]}',
                                PYTHON=str(python), MOCK_LOG=str(self.log), MOCK_STUB=str(self.stub),
                                NINFER_CONTROLLER_ID='fixture-controller', NINFER_TARGET_ID='fixture-target',
                                NINFER_EXPECTED_SOURCE_SHA=head, NINFER_UPSTREAM_BASE_SHA=head,
                                NINFER_ARTIFACT_EXPECTED_SHA256=hashlib.sha256(self.artifact.read_bytes()).hexdigest(),
                                NINFER_TOOLCHAIN_IMAGE_ID='sha256:' + 'a' * 64)
        self.runner = tools / 'run_sm120_mtp3_profile.sh'

    def git(self, *args: str) -> str:
        return subprocess.run(['git', '-C', str(self.source), *args], text=True, capture_output=True, check=True).stdout.strip()

    def run_mode(self, mode: str, **environment: str) -> subprocess.CompletedProcess[str]:
        args = [str(self.runner), mode, str(self.source), str(self.build), str(self.artifact), str(self.prepare)]
        if mode == 'capture':
            args.append(str(self.output))
        return subprocess.run(args, text=True, capture_output=True, env={**self.environment, **environment})

    def calls(self) -> list[list[str]]:
        return [json.loads(line) for line in self.log.read_text(encoding='utf-8').splitlines()] if self.log.exists() else []

    def prepare_ok(self) -> None:
        result = self.run_mode('prepare')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_prepare_builds_current_targets_without_running_gpu(self) -> None:
        self.prepare_ok()
        calls = self.calls()
        self.assertEqual(calls[0][0:2], ['nsys', 'profile'])
        build = next(call for call in calls if '--build' in call)
        self.assertIn('ninfer_qwen3_5_mtp_round_bench', build)
        self.assertIn('ninfer_bench', build)
        self.assertFalse(any('post_mixer' in arg or 'qwen3_6_27b_mtp' in arg for arg in build))
        self.assertFalse(any(call[0] in ('ncu', 'nvidia-smi') for call in calls))
        manifest = json.loads((self.prepare / 'manifest.json').read_text())
        self.assertEqual(manifest['schema_version'], 2)
        self.assertTrue(manifest['source_clean'])

    def test_capture_uses_public_master_cli_and_completes_packet(self) -> None:
        self.prepare_ok()
        result = self.run_mode('capture')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue((self.output / 'packet-complete.txt').is_file())
        manifest = json.loads((self.output / 'manifest.json').read_text())
        self.assertEqual(manifest['schema_version'], 3)
        self.assertEqual(manifest['counter_access_gate'], 'q5-public')
        self.assertEqual(json.loads((self.output / 'ncu/q4-packet/summary.json').read_text())['schema_version'], 2)
        calls = self.calls()
        capture = next(call for call in calls if call[:2] == ['nsys', 'profile'] and '-o' in call)
        self.assertIn('--profile-measured', capture)
        self.assertTrue(any(arg.endswith('/ninfer_bench') for arg in capture))
        self.assertNotIn('--profile-measured', next(call for call in calls if call[0] == 'ninfer_qwen3_5_mtp_round_bench'))
        self.assertTrue((self.output / 'timings/q5-graph.csv').is_file())

    def test_dirty_source_blocks_build(self) -> None:
        (self.source / 'unexpected.txt').write_text('dirty')
        result = self.run_mode('prepare')
        self.assertEqual(result.returncode, 2)
        self.assertIn('completely clean committed worktree', result.stderr)
        self.assertEqual(self.calls(), [])

    def test_wrong_artifact_blocks_build(self) -> None:
        self.artifact.write_bytes(b'other artifact')
        result = self.run_mode('prepare')
        self.assertEqual(result.returncode, 2)
        self.assertIn('artifact SHA-256 differs', result.stderr)
        self.assertEqual(self.calls(), [])

    def test_unsupported_nsys_blocks_configure(self) -> None:
        result = self.run_mode('prepare', MOCK_HELP_FAIL='1')
        self.assertEqual(result.returncode, 64)
        self.assertFalse(any(call[0] == 'cmake' for call in self.calls()))

    def test_counter_permission_failure_blocks_later_gpu_work(self) -> None:
        self.prepare_ok()
        result = self.run_mode('capture', MOCK_COUNTER_DENIED='1')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('performance-counter access is unavailable', result.stderr)
        self.assertFalse((self.output / 'ncu/counter-access-verified.txt').exists())
        self.assertFalse(any(call[0] == 'ninfer_qwen3_5_mtp_round_bench' for call in self.calls()))
        self.assertFalse((self.output / 'packet-complete.txt').exists())

    def test_changed_prepared_binary_blocks_capture(self) -> None:
        self.prepare_ok()
        with (self.build / 'bench/ninfer_q5_linear_add_bench').open('a') as stream:
            stream.write('\n# changed\n')
        result = self.run_mode('capture')
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.output.exists())

    def test_missing_graph_topology_blocks_packet_completion(self) -> None:
        self.prepare_ok()
        result = self.run_mode('capture', MOCK_GRAPH_BAD='1')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('did not expose the CUDA Graph node topology', result.stderr)
        self.assertFalse((self.output / 'packet-complete.txt').exists())


if __name__ == '__main__':
    unittest.main()
