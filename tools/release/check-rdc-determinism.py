#!/usr/bin/env python3
"""Build a bounded production-policy RDC closure twice, never executing it."""

import argparse
import difflib
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess


TARGET = "ninfer_rdc_determinism"
REAL_OBJECT = "src/ops/CMakeFiles/ninfer_ops.dir/normalization/layer_norm.cu.o"


def run(command, *, cwd=None):
    result = subprocess.run(command, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode:
        print(result.stdout.decode(errors="replace"), flush=True)
        result.check_returncode()
    return result.stdout


def digest(data):
    return hashlib.sha256(data).hexdigest()


def sections(data):
    """Read section bytes, including symbol/string tables, from Linux ELF64 LE."""
    if data[:6] != b"\x7fELF\x02\x01":
        return {}
    offset = struct.unpack_from("<Q", data, 40)[0]
    size, count, names_index = struct.unpack_from("<HHH", data, 58)
    headers = [struct.unpack_from("<IIQQQQIIQQ", data, offset + i * size)
               for i in range(count)]
    names_header = headers[names_index]
    names = data[names_header[4]:names_header[4] + names_header[5]]
    result = {}
    for header in headers[1:]:
        name = names[header[0]:].split(b"\0", 1)[0].decode()
        result[name] = {
            "size": header[5],
            "sha256": digest(data[header[4]:header[4] + header[5]])
            if header[1] != 8 else None,  # SHT_NOBITS has no file payload.
        }
    return result


def stage(name):
    if name.endswith("cmake_device_link.o"):
        return "device_link"
    if name.endswith(".o"):
        return "compile"
    if name.endswith(".a"):
        return "archive"
    return "host_link"


def compare(left, right, evidence):
    names_left = {p.relative_to(left).as_posix() for p in left.rglob("*") if p.is_file()}
    names_right = {p.relative_to(right).as_posix() for p in right.rglob("*") if p.is_file()}
    rows = []
    for name in sorted(names_left & names_right):
        a, b = (left / name).read_bytes(), (right / name).read_bytes()
        sa, sb = sections(a), sections(b)
        row = {
            "name": name, "stage": stage(name), "equal": a == b,
            "sha256": [digest(a), digest(b)], "bytes": [len(a), len(b)],
            "first_difference": next((i for i, (x, y) in enumerate(zip(a, b)) if x != y),
                                     min(len(a), len(b))) if a != b else None,
            "differing_sections": {key: [sa.get(key), sb.get(key)]
                                   for key in sorted(sa.keys() | sb.keys())
                                   if sa.get(key) != sb.get(key)},
        }
        rows.append(row)
        if a != b:
            command = ["ar", "tv"] if name.endswith(".a") else ["readelf", "-W", "-s", "-n"]
            first = run([*command, str(left / name)]).decode(errors="replace")
            second = run([*command, str(right / name)]).decode(errors="replace")
            report = evidence / (name.replace("/", "_") + ".diff")
            report.write_text("".join(difflib.unified_diff(
                first.splitlines(True), second.splitlines(True), fromfile="round1", tofile="round2")))
    return {
        "equal": bool(rows) and names_left == names_right and all(r["equal"] for r in rows),
        "missing": sorted(names_left - names_right), "extra": sorted(names_right - names_left),
        "artifacts": rows,
    }


def capture(build, destination):
    names = [Path("tests") / TARGET, Path("tests/libninfer_rdc_fixture.a"), Path(REAL_OBJECT)]
    names += [p.relative_to(build) for p in (build / "tests/CMakeFiles").rglob("*.o")
              if "ninfer_rdc_" in p.as_posix()]
    for name in names:
        target = destination / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(build / name, target)


def probes(source, build, evidence):
    """Failure-only attribution: test documented seeds and temporary-name controls."""
    nvcc = shutil.which("nvcc")
    (evidence / "cudafe-help.txt").write_bytes(
        subprocess.run([str(Path(nvcc).parent / "cudafe++"), "--help"],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT).stdout)
    results = {}
    variants = {
        "seed64_keep": ["--frandom-seed=0x123456789abcdef0", "--keep"],
        "seed32_keep": ["--frandom-seed=12345", "--keep"],
        "seed32_objdir": ["--frandom-seed=12345", "--objdir-as-tempdir"],
        "seed32_keep_epoch": ["--frandom-seed=12345", "--keep"],
    }
    for name, flags in variants.items():
        outputs = []
        logs = []
        root = build / "probe"
        for _ in range(2):
            shutil.rmtree(root, ignore_errors=True)
            root.mkdir()
            env = dict(os.environ)
            if name.endswith("_epoch"):
                env["SOURCE_DATE_EPOCH"] = "1"
            command = [nvcc, "-std=c++20", "-arch=sm_120a", "-rdc=true", "-lineinfo",
                       *flags, "-c", str(source / "tests/cmake/rdc/left/kernel.cu"), "-o", "kernel.o"]
            result = subprocess.run(command, cwd=root, env=env, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT)
            logs.append(result.stdout.decode(errors="replace"))
            outputs.append({p.name: digest(p.read_bytes()) for p in root.iterdir()
                            if p.is_file() and p.suffix in (".o", ".ptx", ".module_id")})
        results[name] = {"rounds": outputs, "equal": bool(outputs[0]) and outputs[0] == outputs[1],
                         "logs": logs}
    (evidence / "probes.json").write_text(json.dumps(results, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work", type=Path, required=True, help="new, disposable output directory")
    args = parser.parse_args()
    root = args.work.resolve()
    root.mkdir(parents=True, exist_ok=False)
    source = Path(__file__).resolve().parents[2]
    build = root / "build"
    evidence = root / "evidence"
    evidence.mkdir()
    os.environ["CCACHE_DISABLE"] = "1"
    configuration = ["cmake", "-S", str(source), "-B", str(build), "-G", "Ninja",
                     "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_CUDA_ARCHITECTURES=120a",
                     "-DCMAKE_C_COMPILER_LAUNCHER=", "-DCMAKE_CXX_COMPILER_LAUNCHER=",
                     "-DCMAKE_CUDA_COMPILER_LAUNCHER=", "-DBUILD_TESTING=ON",
                     "-DNINFER_BUILD_APPS=OFF", "-DNINFER_BUILD_BENCHMARKS=OFF",
                     "-DPython3_EXECUTABLE=/usr/bin/python3",
                     "-DNINFER_BUILD_PROFILE=ci-rdc-determinism"]
    compilation = ["cmake", "--build", str(build), "--parallel", "2", "--verbose",
                   "--target", TARGET, REAL_OBJECT]
    (evidence / "toolchain.txt").write_bytes(run(["nvcc", "--version"]) + run(["c++", "--version"]))
    for number in (1, 2):
        if build.exists():
            shutil.rmtree(build)
        (evidence / f"configure{number}.log").write_bytes(run(configuration))
        (evidence / f"build{number}.log").write_bytes(run(compilation))
        capture(build, root / f"round{number}")
    result = compare(root / "round1", root / "round2", evidence)
    # Rebuild only the downstream stages with the second round's fixed objects.
    # This separates propagated compile differences from independent link/archive drift.
    for path in (build / "tests/libninfer_rdc_fixture.a", build / f"tests/{TARGET}"):
        path.unlink()
    for path in (build / "tests/CMakeFiles/ninfer_rdc_fixture.dir").rglob("cmake_device_link.o"):
        path.unlink()
    (evidence / "relink.log").write_bytes(run(compilation))
    capture(build, root / "relink")
    result["fixed_input_relink"] = compare(root / "round2", root / "relink", evidence)
    result["ccache_disabled"] = True
    result["same_source_and_build_paths"] = True
    (evidence / "comparison.json").write_text(json.dumps(result, indent=2) + "\n")
    for row in result["artifacts"]:
        print(f'{row["stage"]}: {row["name"]}: {"identical" if row["equal"] else "DIFFERENT"}; '
              f'sections={",".join(row["differing_sections"])}', flush=True)
    if not result["equal"]:
        probes(source, build, evidence)
    if not result["equal"] or not result["fixed_input_relink"]["equal"]:
        raise SystemExit("RDC double build differs; see comparison.json and symbol/section evidence")


if __name__ == "__main__":
    main()
