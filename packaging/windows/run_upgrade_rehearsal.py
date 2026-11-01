#!/usr/bin/env python3
"""Run all three repository upgrade scenarios through an explicit private transport."""
import argparse
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.qualification import driver_state as store

SCENARIOS = ('running', 'rollback', 'repair')


def execute(config_path: Path, attempt: str) -> None:
    config = store.parse_object(config_path.read_text(encoding='utf-8-sig'))
    store.require_disjoint_logs(config['evidence_parent'], config['immutable_roots'])
    directory = store.claim_attempt(Path(config['evidence_parent']), attempt)
    for scenario in SCENARIOS:
        state_path = directory / (scenario + '.state.json')
        receipt_path = directory / (scenario + '.json')
        if scenario != SCENARIOS[0]:
            prior = SCENARIOS[SCENARIOS.index(scenario) - 1]
            store.completed_receipt(directory / (prior + '.state.json'), directory / (prior + '.json'))
        state = {'status': 'running', 'scenario': scenario}
        store.atomic_json(state_path, state)
        # command ends with the repository PS driver and its private -Config argument;
        # SSH configuration owns hosts/ports/auth. Lock arguments are explicit, not shell text.
        command = [*config['lock_command'], *config['command'], '-Attempt', attempt, '-Scenario', scenario]
        if not config['lock_command'] or not config['command'] or any(not isinstance(x, str) or not x for x in command):
            raise ValueError('lock and transport must be nonempty argument arrays')
        try:
            try:
                result = subprocess.run(command, stdin=subprocess.DEVNULL, capture_output=True, text=True,
                                        timeout=config['timeout_seconds'])
            except subprocess.TimeoutExpired as error:
                # TimeoutExpired can carry bytes even with text=True.
                for stream, partial in (('stdout', error.stdout), ('stderr', error.stderr)):
                    log_path = directory / (scenario + '.' + stream + '.log')
                    # Retain text partials as UTF-8 bytes without platform newline translation.
                    payload = partial if isinstance(partial, bytes) else (partial or '').encode('utf-8')
                    log_path.write_bytes(payload)
                raise
            (directory / (scenario + '.stdout.log')).write_text(result.stdout)
            (directory / (scenario + '.stderr.log')).write_text(result.stderr)
            result.check_returncode()
            receipt = store.parse_object(result.stdout, compact=True)
            rows = store.require_census(receipt['census'])
            if (receipt.get('scenario') != scenario or receipt.get('status') != 'passed'
                    or receipt.get('restored') is not True or rows):
                raise ValueError('upgrade scenario lacks a completed, restored terminal receipt')
            store.publish_completed(state_path, receipt_path, state,
                                    {**receipt, 'remote_state_sha256': receipt['state_sha256']})
        except BaseException:
            state['status'] = 'failed'
            store.atomic_json(state_path, state)
            raise


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', type=Path, required=True)
    parser.add_argument('--attempt', required=True)
    args = parser.parse_args()
    execute(args.config, args.attempt)
