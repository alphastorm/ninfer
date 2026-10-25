#!/usr/bin/env bash
set -Eeuo pipefail

[[ $# -eq 3 ]] || { echo "usage: $0 SOURCE_ROOT BUILD_DIR OUTPUT_DIR" >&2; exit 2; }
source_root=$(cd "$1" && pwd -P)
build_dir=$(cd "$2" && pwd -P)
output_dir=$3
: "${NINFER_CONTROLLER_ID:?set NINFER_CONTROLLER_ID before profiling}"
: "${NINFER_TARGET_ID:?set NINFER_TARGET_ID before profiling}"
[[ $NINFER_CONTROLLER_ID != "$NINFER_TARGET_ID" ]] || { echo 'controller and target identities must differ' >&2; exit 2; }
[[ ! -e $output_dir ]] || { echo 'output directory already exists' >&2; exit 2; }
python=${PYTHON:-python3.11}
for command in cmake cuobjdump git ncu nvidia-smi nvcc sha256sum "$python"; do
  command -v "$command" >/dev/null || { echo "required profiling command is unavailable: $command" >&2; exit 2; }
done
bench="$build_dir/bench/ninfer_q4_linear_swiglu_bench"
[[ -x $bench ]] || { echo "Q4 LinearSwiGLU benchmark is not executable: $bench" >&2; exit 2; }
grep -Fqx "CMAKE_HOME_DIRECTORY:INTERNAL=$source_root" "$build_dir/CMakeCache.txt" || { echo 'build directory is not configured from SOURCE_ROOT' >&2; exit 2; }
mkdir -p "$output_dir"
output_dir=$(cd "$output_dir" && pwd -P)
{
  printf 'source_sha\t%s\n' "$(git -C "$source_root" rev-parse HEAD)"
  printf 'benchmark_sha256\t%s\n' "$(sha256sum "$bench" | cut -d' ' -f1)"
  printf 'controller_id\t%s\ntarget_id\t%s\n' "$NINFER_CONTROLLER_ID" "$NINFER_TARGET_ID"
} >"$output_dir/identity.tsv"
cmake -N -LA "$build_dir" >"$output_dir/cmake-cache.txt" 2>&1
nvcc --version >"$output_dir/nvcc.txt" 2>&1
cuobjdump --list-elf "$bench" >"$output_dir/cubins.txt" 2>&1
grep -Eq '(^|[^[:alnum:]_])sm_120a([^[:alnum:]_]|$)' "$output_dir/cubins.txt" || { echo 'benchmark does not contain a native sm_120a cubin' >&2; exit 1; }
cuobjdump --dump-resource-usage "$bench" >"$output_dir/benchmark-resources.txt" 2>&1
nvidia-smi --query-gpu=name,uuid,driver_version,temperature.gpu --format=csv >"$output_dir/gpu-before.csv"
"$bench" --t-sweep 1,4 --execution eager --warmup 5 --repeat 30 --csv-out "$output_dir/timing-public.csv" >"$output_dir/timing-public.log" 2>&1
# Master exposes only the public Op. Its CSV proves both requested widths ran;
# geometry comes from that benchmark's fixed public contract, not an invented route API.
"$python" - "$output_dir/timing-public.csv" "$output_dir/shape-manifest.jsonl" <<'PY'
import csv
import json
import sys
from pathlib import Path
rows = list(csv.DictReader(Path(sys.argv[1]).open(encoding='utf-8')))
if len(rows) != 2 or {row['t'] for row in rows} != {'1', '4'} or any(row['execution'] != 'eager' for row in rows):
    raise SystemExit('public Q4 benchmark did not measure exactly eager T=1,4')
records = [dict(schema_version=2, kernel_family='linear_swiglu_q4', route='public', execution='eager', gate_up_n=34816, output_n=17408, k=5120, group_size=64, m=int(row['t'])) for row in rows]
Path(sys.argv[2]).write_text(''.join(json.dumps(row, sort_keys=True) + '\n' for row in records), encoding='utf-8')
PY
sections=$(ncu --list-sections 2>&1)
printf '%s\n' "$sections" >"$output_dir/ncu-sections.txt"
section_args=()
for section in LaunchStats SpeedOfLight MemoryWorkloadAnalysis; do
  grep -Eq "(^|[[:space:]])${section}([[:space:]]|$)" <<<"$sections" || { echo "required NCU section is unavailable: $section" >&2; exit 1; }
  section_args+=(--section "$section")
done
capture() {
  local width=$1 kernel=$2 base="$output_dir/q4-m$1"
  ncu --target-processes all --kernel-name-base function --kernel-name "regex:.*${kernel}.*" \
    "${section_args[@]}" --launch-count 1 --force-overwrite --export "$base" \
    "$bench" --t-sweep "$width" --profile >"$base.log" 2>&1
  [[ -s $base.ncu-rep ]] || { echo 'NCU emitted no report' >&2; return 1; }
  ncu --import "$base.ncu-rep" --csv --page raw >"$base.csv" 2>"$base-export.log"
}
capture 1 q4_linear_swiglu_gemv_pair_kernel
capture 4 q4_a16_sliced_k_mma_kernel
"$python" "$source_root/tools/bench/summarize_ncu_q4.py" \
  --m1-csv "$output_dir/q4-m1.csv" --m4-csv "$output_dir/q4-m4.csv" \
  --shape-manifest "$output_dir/shape-manifest.jsonl" --output "$output_dir/summary.json"
nvidia-smi --query-gpu=name,uuid,driver_version,temperature.gpu --format=csv >"$output_dir/gpu-after.csv"
printf 'profile packet: %s\n' "$output_dir"
