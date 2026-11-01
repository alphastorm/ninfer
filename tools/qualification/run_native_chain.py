#!/usr/bin/env python3
"""Run the native chain in isolated, locked stages; inventory stays outside the repo."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.qualification import driver_state as store
from tools.qualification import qualify_native as native

GROUPS = {
    'cpu': ('preflight', 'neutral_build', 'private_path_scan', 'package'),
    'protocol': ('transfer_install', 'protocol'),
    'correctness': ('pressure_protocol', 'context_128k', 'restart', 'rollback'),
    'security': ('security',),
    'finish': ('omp', 'benchmark_c1', 'receipt', 'restore'),
}
METHODS = {'private_path_scan': 'private_scan', 'context_128k': 'context', 'benchmark_c1': 'benchmark'}


class ChainOrchestrator(native.Orchestrator):
    def remote_json(self, host: str, script: str, timeout: int = 600) -> dict:
        if host == self.config.target:
            script = native.TARGET_CONTROLLER_GUARD + script
        output = native.remote_ps(host, script, timeout=timeout)
        try:
            return store.parse_object(output)
        except ValueError:
            # Progress may precede the object, never follow it. Start at the FIRST
            # JSON-looking line so a nested child cannot rescue a truncated document.
            lines = output.splitlines()
            for index, line in enumerate(lines):
                if line.lstrip().startswith(('{', '[')):
                    return store.parse_object('\n'.join(lines[index:]))
            raise ValueError('command emitted no JSON receipt')

    def retire_scratch(self) -> dict:
        return self.remote_json(self.config.target, f"""
$ErrorActionPreference='Stop'
$root={native.ps_quote(self.state_root)}
$task={native.ps_quote(self.task_name)}
if(Test-Path (Join-Path $root 'state.json')){{
  $state=Get-Content (Join-Path $root 'state.json') -Raw|ConvertFrom-Json
  if([string]$state.task_name -cne $task){{throw 'refusing retirement of a foreign task'}}
  &(Join-Path $root 'Control-Release.ps1') -Action Uninstall -StateRoot $root|Out-Null
}}
if((Test-Path $root) -or $null -ne (Get-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue)){{
  throw 'qualification scratch retirement incomplete'
}}
[Console]::Out.WriteLine('{{"status":"passed","scratch_retired":true}}')
""", timeout=900)


def prior_phases(part: str) -> list[str]:
    names = []
    for group, phases in GROUPS.items():
        if group == part:
            return names
        names.extend(phases)
    raise ValueError('unknown chain stage')


def finalize(orchestrator) -> None:
    store.require_phases(orchestrator.state, orchestrator.config.state_dir, list(native.PHASES))
    orchestrator.state['status'] = 'passed'
    orchestrator._save()  # The public receipt must never precede the state it claims.
    persisted = store.parse_object(orchestrator.state_path.read_text(encoding='utf-8'))
    summary = store.parse_object((orchestrator.config.state_dir / 'qualification-summary.json').read_text())
    lane = native.stage_lane_qualification(persisted, summary, orchestrator.lane)
    native.atomic_json(orchestrator.config.state_dir / (orchestrator.lane.name + '.json'), lane)


def run_part(config_path: Path, attempt: str, part: str) -> None:
    raw = config_path.read_bytes()
    settings = store.parse_object(raw.decode('utf-8-sig'))
    parent = Path(settings['evidence_parent'])
    directory = store.claim_attempt(parent, attempt) if part == 'cpu' else parent / attempt
    binding = hashlib.sha256(raw + Path(__file__).read_bytes() + Path(native.__file__).read_bytes()).hexdigest()
    previous = list(GROUPS)[:list(GROUPS).index(part)]
    for name in previous:
        receipt = store.completed_receipt(directory / (name + '.state.json'), directory / (name + '.json'))
        if receipt.get('binding') != binding:
            raise ValueError('continuation inputs or driver changed; use a new attempt')
    # An interrupted or failed group is evidence, never an implicit retry opportunity.
    with (directory / (part + '.started')).open('x') as stream:
        stream.write(binding + '\n')
    values = {key: value.replace('{attempt}', attempt) if isinstance(value, str) else value
              for key, value in settings['native'].items()}
    for key in ('source_root', 'long_fixture', 'transfer_tool'):
        if values.get(key) is not None:
            values[key] = Path(values[key])
    values.update(state_dir=directory / 'native', resume=False, dry_run=False,
                  through_phase=None, upgrade_orchestrator_state=False, scratch_task_suffix=attempt)
    config = native.Config(**values)
    orchestrator = ChainOrchestrator(config)
    builder = settings['builder_parent'].rstrip('/\\') + '/' + attempt + '-builder'
    target = settings['target_parent'].rstrip('/\\') + '/' + attempt + '-target'
    store.require_disjoint_logs(str(directory), [str(config.source_root)])
    orchestrator.builder_root = builder
    for attr, leaf in (('builder_source', 'source'), ('builder_build', 'build'),
                       ('builder_bin', 'neutral-bin'), ('builder_out_a', 'package-a'),
                       ('builder_out_b', 'package-b'), ('builder_scratch_a', 'scratch-a'),
                       ('builder_scratch_b', 'scratch-b'), ('builder_script', 'qualify_native.py')):
        setattr(orchestrator, attr, builder + '/' + leaf)
    orchestrator.target_root = target
    scratch = native.PureWindowsPath(config.scratch_state_root)
    if ('..' in scratch.parts or not scratch.is_absolute()
            or not scratch.is_relative_to(native.PureWindowsPath(target))):
        raise ValueError('scratch state root must be inside the fresh target attempt')
    if any(not native.PureWindowsPath(root).is_absolute() or '..' in native.PureWindowsPath(root).parts
           for root in (builder, target)):
        raise ValueError('attempt parents must be absolute Windows paths without traversal')
    store.require_phases(orchestrator.state, config.state_dir, prior_phases(part))
    state_path, receipt_path = directory / (part + '.state.json'), directory / (part + '.json')
    stage = {'status': 'running', 'part': part, 'binding': binding}
    store.atomic_json(state_path, stage)
    if part == 'cpu':
        # A namespace refusal must not enter cleanup against the pre-existing namespace.
        for host, root in ((config.builder, builder), (config.target, target)):
            orchestrator.remote_json(host, f"""
$ErrorActionPreference='Stop'
$root={native.ps_quote(root)}
if(Test-Path -LiteralPath $root){{throw 'attempt namespace already exists'}}
New-Item -ItemType Directory -Path $root -ErrorAction Stop|Out-Null
[Console]::Out.WriteLine('{{"status":"passed"}}')
""")
    try:
        for name in GROUPS[part]:
            orchestrator.phase(name, getattr(orchestrator, METHODS.get(name, name)))
            if name == 'package':
                package = orchestrator.state['phases'][name]['receipt']['package']['sha256']
                if package != settings['expected_package_sha256']:
                    raise ValueError('qualification package differs from the frozen candidate')
        if part == 'finish':
            orchestrator.state['scratch_retirement'] = orchestrator.retire_scratch()
            finalize(orchestrator)
        else:
            orchestrator.cleanup()
            if orchestrator.state['restoration'].get('status') != 'passed':
                raise ValueError('inter-stage restoration failed')
            orchestrator.state['status'] = 'paused'
            orchestrator._save()
        store.publish_completed(state_path, receipt_path, stage,
                                {'status': 'passed', 'part': part, 'binding': binding})
    except BaseException:
        orchestrator.cleanup()
        stage['status'] = 'failed'
        store.atomic_json(state_path, stage)
        raise


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', type=Path, required=True, help='private JSON inventory')
    parser.add_argument('--attempt', required=True)
    parser.add_argument('--continue-from', choices=GROUPS, default='cpu')
    parser.add_argument('--part', choices=GROUPS, help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.part:
        run_part(args.config.resolve(), args.attempt, args.part)
        return
    settings = store.parse_object(args.config.read_text(encoding='utf-8-sig'))
    for part in GROUPS:
        lock = settings['lock_commands'][part]
        if not isinstance(lock, list) or not lock or any(not isinstance(x, str) or not x for x in lock):
            raise ValueError('each stage requires an explicit lock command argument array')
    for part in list(GROUPS)[list(GROUPS).index(args.continue_from):]:
        lock = settings['lock_commands'][part]
        command = [*lock, sys.executable, '-B', str(Path(__file__).resolve()), '--config',
                   str(args.config.resolve()), '--attempt', args.attempt, '--part', part]
        subprocess.run(command, check=True, stdin=subprocess.DEVNULL)


if __name__ == '__main__':
    main()
