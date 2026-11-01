"""File-only publication and input-boundary contracts for release drivers."""
from __future__ import annotations

import hashlib
import json
import ntpath
import os
from pathlib import Path
import re


def claim_attempt(parent: Path, attempt: str) -> Path:
    if re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9-]{0,23}", attempt) is None:
        raise ValueError("attempt must be 1-24 letters, digits or hyphens")
    path = parent / attempt
    path.mkdir(parents=True, exist_ok=False)
    return path


def parse_object(text: str, *, compact: bool = False) -> dict:
    text = text.lstrip('\ufeff').strip()
    if compact and len(text.splitlines()) != 1:
        raise ValueError("compact consumer requires exactly one JSON line")
    value = json.loads(text)
    if not isinstance(value, dict):
        raise ValueError("JSON receipt must be an object")
    return value


def require_census(value: dict) -> list[dict]:
    rows = value.get('rows')
    if not isinstance(rows, list) or any(not isinstance(row, dict) for row in rows):
        raise ValueError("census rows must be an array of objects, including zero/one rows")
    return rows


def require_disjoint_logs(logs: str, roots: list[str]) -> None:
    # Windows path semantics also apply when a POSIX controller validates remote paths.
    def normalized(value):
        if os.name == 'nt' or ntpath.splitdrive(value)[0] or '\\' in value:
            return ntpath, ntpath.normcase(ntpath.abspath(value))
        return os.path, str(Path(value).resolve())
    flavor, log = normalized(logs)
    for root in roots:
        source_flavor, source = normalized(root)
        if source_flavor is not flavor:
            continue
        try:
            common = flavor.commonpath((log, source))
        except ValueError:  # Different Windows drives cannot overlap.
            continue
        if common in (log, source):
            raise ValueError("mutable logs and immutable input roots must be disjoint")


def atomic_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + '.tmp')
    with temporary.open('w', encoding='utf-8', newline='\n') as stream:
        json.dump(value, stream, sort_keys=True, separators=(',', ':'))
        stream.write('\n')
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)


def file_sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def publish_completed(state_path: Path, receipt_path: Path, state: dict, receipt: dict) -> None:
    if receipt.get('status') != 'passed':
        raise ValueError('cannot publish an incomplete receipt')
    state['status'] = 'passed'
    atomic_json(state_path, state)
    atomic_json(receipt_path, {**receipt, 'state_sha256': file_sha256(state_path)})


def completed_receipt(state_path: Path, receipt_path: Path) -> dict:
    state = parse_object(state_path.read_text(encoding='utf-8-sig'))
    receipt = parse_object(receipt_path.read_text(encoding='utf-8-sig'))
    if (state.get('status') != 'passed' or receipt.get('status') != 'passed'
            or receipt.get('state_sha256') != file_sha256(state_path)):
        raise ValueError('continuation requires a completed receipt bound to persisted state')
    return receipt


def require_phases(state: dict, directory: Path, names: list[str]) -> None:
    for name in names:
        phase = state.get('phases', {}).get(name, {})
        path = directory / 'receipts' / (name + '.json')
        if (phase.get('status') != 'passed' or not path.is_file()
                or phase.get('receipt_sha256') != file_sha256(path)
                or phase.get('receipt') != parse_object(path.read_text(encoding='utf-8-sig'))):
            raise ValueError('incomplete or changed phase receipt: ' + name)
