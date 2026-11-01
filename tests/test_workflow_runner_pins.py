"""Keep host-installed CUDA on explicit GitHub runner images (stdlib only)."""
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
JOB = re.compile(r"^  ([A-Za-z_][A-Za-z0-9_-]*):\s*(?:#.*)?$", re.MULTILINE)
CUDA_INSTALL = re.compile(
    r"cuda-toolkit(?:-\d|[ \t])|cuda-keyring|network_installers/cuda_|setup-cuda@",
    re.IGNORECASE,
)
FLOATING_RUNNER = re.compile(r"\b[A-Za-z0-9_-]+-latest\b")


def cuda_jobs_with_floating_runners(source):
    """Inspect the repository's block-style job/runner declarations, not YAML values."""
    jobs = re.search(r"^jobs:\s*$", source, re.MULTILINE)
    if jobs is None:
        return []
    source = source[jobs.end():]
    headers = list(JOB.finditer(source))
    failures = []
    for index, header in enumerate(headers):
        end = headers[index + 1].start() if index + 1 < len(headers) else len(source)
        body = source[header.end():end]
        if not CUDA_INSTALL.search(body):
            continue
        runner = re.search(r"^    runs-on:([^\n]*(?:\n {6,}[^\n]*)*)", body, re.MULTILINE)
        if runner is None:
            raise ValueError(f"CUDA job {header[1]} has no block-style runs-on declaration")
        labels = runner[1]
        for dimension in re.findall(r"\bmatrix\.([A-Za-z_][A-Za-z0-9_]*)", labels):
            values = re.findall(
                rf"^ {{8,}}(?:- )?{re.escape(dimension)}:([^\n]*(?:\n {{10,}}[^\n]*)*)",
                body, re.MULTILINE,
            )
            if not values:
                raise ValueError(f"CUDA job {header[1]} has no runner matrix {dimension}")
            labels += "\n" + "\n".join(values)
        if FLOATING_RUNNER.search(labels):
            failures.append(header[1])
    return failures


class WorkflowRunnerPinsTests(unittest.TestCase):
    def test_cuda_installing_jobs_use_fixed_runners(self):
        workflows = sorted(path for path in (ROOT / ".github/workflows").iterdir()
                           if path.suffix in (".yml", ".yaml"))
        self.assertTrue(workflows, "workflow catalog is empty")
        failures = []
        for path in workflows:
            failures.extend(f"{path.name}:{job}" for job in
                            cuda_jobs_with_floating_runners(path.read_text(encoding="utf-8")))
        self.assertEqual(failures, [], "CUDA installers require fixed runner images")

    def test_floating_cuda_job_is_rejected_without_leaking_into_other_jobs(self):
        source = """jobs:
  python:
    runs-on: ubuntu-latest
    steps:
      - run: python -m unittest
  cuda:
    runs-on: ubuntu-latest
    steps:
      - run: sudo apt-get install cuda-toolkit-13-1
  windows:
    runs-on: windows-2022
    steps:
      - run: download network_installers/cuda_13_windows.exe
"""
        self.assertEqual(cuda_jobs_with_floating_runners(source), ["cuda"])

    def test_fixed_cuda_runner_and_non_cuda_latest_are_allowed(self):
        source = """jobs:
  cuda:
    runs-on: ubuntu-24.04
    steps:
      - run: sudo apt-get install cuda-toolkit-13-1
  python:
    runs-on: ubuntu-latest
    steps:
      - run: python -m unittest
"""
        self.assertEqual(cuda_jobs_with_floating_runners(source), [])

    def test_runner_lists_and_matrices_cannot_hide_latest(self):
        for runner, strategy in (
            ("[self-hosted, ubuntu-latest]", ""),
            ("\n      - self-hosted\n      - ubuntu-latest", ""),
            ("${{ matrix.os }}", "    strategy:\n      matrix:\n        os: [ubuntu-24.04, ubuntu-latest]\n"),
        ):
            with self.subTest(runner=runner):
                source = ("jobs:\n  cuda:\n    runs-on: " + runner + "\n" + strategy +
                          "    steps:\n      - run: sudo apt-get install cuda-toolkit-13-1\n")
                self.assertEqual(cuda_jobs_with_floating_runners(source), ["cuda"])


if __name__ == "__main__":
    unittest.main()
